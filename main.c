#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define sleep(x) Sleep((x) * 1000)
#define popen _popen
#define pclose _pclose
#define mkdir(path, mode) _mkdir(path)
#else
#include <ncurses.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "cjson/cJSON.h"
#include "colors.h"

#define MAX_TUNNELS 32
#define LOG_DIR "logs"
#define CONFIG_FILE "config.json"
#define MAX_CMD_LEN 512
#define MAX_NAME_LEN 64
#define MAX_HOST_LEN 128
#define MAX_PATH_LEN 256

typedef enum {
    TUNNEL_STOPPED = 0,
    TUNNEL_STARTING,
    TUNNEL_RUNNING,
    TUNNEL_ERROR,
    TUNNEL_AUTH_ERROR,  // SSH key/authentication problems
    TUNNEL_PORT_ERROR,  // Port already in use
    TUNNEL_RECONNECTING
} tunnel_status_t;

typedef enum {
    TUNNEL_TYPE_FORWARD = 0,  // -L (default) - Remote service accessible locally
    TUNNEL_TYPE_REVERSE = 1   // -R - Local service accessible remotely
} tunnel_type_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char host[MAX_HOST_LEN];
    int port;
    char user[MAX_NAME_LEN];
    char ssh_key[MAX_PATH_LEN];  // SSH private key file path
    tunnel_type_t type;          // Forward (-L) or Reverse (-R)
    int local_port;
    char remote_host[MAX_HOST_LEN];
    int remote_port;
    int reconnect_delay;
    
    // Runtime state
    int restart_count;
    tunnel_status_t status;
    time_t last_restart;
    pthread_t thread;
    FILE *log;
    pid_t ssh_pid;
    int should_run;
} tunnel_t;

typedef struct {
    tunnel_t tunnels[MAX_TUNNELS];
    int count;
    pthread_mutex_t mutex;
    volatile int running;
} tunnel_manager_t;

static tunnel_manager_t manager = {0};

// Forward declarations
void *tunnel_worker(void *arg);
void cleanup_manager(void);
void signal_handler(int sig);
int load_config(const char *filename);
void save_config(const char *filename);
void start_all_tunnels(void);
void stop_all_tunnels(void);
void start_tunnel_by_name(const char *name);
void stop_tunnel_by_name(const char *name);
void reset_tunnel_by_name(const char *name);
void add_tunnel_interactive(void);
void print_status(void);
void interactive_mode(void);
void log_tunnel_event(tunnel_t *tunnel, const char *event);
int test_tunnel_connectivity(tunnel_t *tunnel);

void log_tunnel_event(tunnel_t *tunnel, const char *event) {
    if (!tunnel->log) return;
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    fprintf(tunnel->log, "[%s] [Restart #%d] %s\n", 
            timestamp, tunnel->restart_count, event);
    fflush(tunnel->log);
    
    // Auch auf stderr für bessere Screen-Integration
    fprintf(stderr, "%s[%s]%s %s[%s]%s %s\n", 
            C_DIM, timestamp, C_RESET,
            C_CYAN, tunnel->name, C_RESET, event);
}

int test_tunnel_connectivity(tunnel_t *tunnel) {
    // Test connectivity based on tunnel type
    if (tunnel->type == TUNNEL_TYPE_REVERSE) {
        // For reverse tunnels, we can only test if the local service is running
        // The remote port availability would need to be tested from the remote side
        printf("%s🔧 Reverse tunnel test: Checking if local service on port %d is accessible%s\n", 
               C_INFO, tunnel->local_port, C_RESET);
    }
    
    // Simple test: try to connect to the relevant port
#ifdef _WIN32
    SOCKET sock;
    struct sockaddr_in addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return 0;
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tunnel->local_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    closesocket(sock);
    return (result == 0);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tunnel->local_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    return (result == 0);
#endif
}

void *tunnel_worker(void *arg) {
    tunnel_t *tunnel = (tunnel_t *)arg;
    char cmd[MAX_CMD_LEN];
    FILE *ssh_proc = NULL;
    char output_buffer[512];
    
    while (tunnel->should_run && manager.running) {
        pthread_mutex_lock(&manager.mutex);
        tunnel->status = TUNNEL_STARTING;
        tunnel->restart_count++;
        tunnel->last_restart = time(NULL);
        pthread_mutex_unlock(&manager.mutex);
        
        log_tunnel_event(tunnel, "🚀 Starting SSH tunnel");
        
        // Build SSH command with Forward (-L) or Reverse (-R) tunneling
        if (tunnel->type == TUNNEL_TYPE_REVERSE) {
            // Reverse tunnel: Local service accessible on remote server
            snprintf(cmd, sizeof(cmd),
                    "ssh -i %s -N -R %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no 2>&1",
                    tunnel->ssh_key, tunnel->remote_port, tunnel->remote_host, tunnel->local_port,
                    tunnel->user, tunnel->host, tunnel->port);
        } else {
            // Forward tunnel: Remote service accessible locally (default)
            snprintf(cmd, sizeof(cmd),
                    "ssh -i %s -N -L %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no 2>&1",
                    tunnel->ssh_key, tunnel->local_port, tunnel->remote_host, tunnel->remote_port,
                    tunnel->user, tunnel->host, tunnel->port);
        }
        
        log_tunnel_event(tunnel, "📡 Executing SSH command with BatchMode");
        
        // Start SSH process
        ssh_proc = popen(cmd, "r");
        if (!ssh_proc) {
            pthread_mutex_lock(&manager.mutex);
            tunnel->status = TUNNEL_ERROR;
            pthread_mutex_unlock(&manager.mutex);
            
            log_tunnel_event(tunnel, "❌ Failed to start SSH process");
            sleep(tunnel->reconnect_delay);
            continue;
        }
        
        // Give SSH a moment to establish the connection
        sleep(2);
        
        // Read initial output to check for immediate errors
        int has_output = 0;
        char all_output[1024] = {0};  // Collect all output for better debugging
        
        while (fgets(output_buffer, sizeof(output_buffer), ssh_proc)) {
            has_output = 1;
            output_buffer[strcspn(output_buffer, "\r\n")] = 0; // Remove newlines
            if (strlen(output_buffer) > 0) {
                // Append to all_output for logging
                if (strlen(all_output) + strlen(output_buffer) < sizeof(all_output) - 10) {
                    if (strlen(all_output) > 0) strcat(all_output, " | ");
                    strcat(all_output, output_buffer);
                }
                
                char log_msg[1024];
                snprintf(log_msg, sizeof(log_msg), "🔍 SSH output: %s", output_buffer);
                log_tunnel_event(tunnel, log_msg);
            }
        }
        
        // Log complete output for reverse tunnel debugging
        if (has_output && tunnel->type == TUNNEL_TYPE_REVERSE) {
            char debug_msg[1536];
            snprintf(debug_msg, sizeof(debug_msg), "🔧 Complete SSH output for reverse tunnel: %s", all_output);
            log_tunnel_event(tunnel, debug_msg);
        }
        
        // If we got immediate output, it's likely an error
        if (has_output && (strstr(all_output, "Permission denied") || 
                          strstr(all_output, "Connection refused") ||
                          strstr(all_output, "Host key verification failed") ||
                          strstr(all_output, "No such file") ||
                          strstr(all_output, "Authentication failed") ||
                          strstr(all_output, "Could not resolve hostname") ||
                          strstr(all_output, "bind: Address already in use") ||
                          strstr(all_output, "Permissions") ||
                          strstr(all_output, "too open") ||
                          strstr(all_output, "remote port forwarding failed") ||
                          strstr(all_output, "Warning: remote port forwarding failed") ||
                          strstr(all_output, "cannot listen to port") ||
                          strstr(all_output, "bind: Cannot assign requested address"))) {
            
            pthread_mutex_lock(&manager.mutex);
            if (strstr(all_output, "Permission denied") || strstr(all_output, "Authentication failed") || 
                strstr(all_output, "Permissions") || strstr(all_output, "too open")) {
                tunnel->status = TUNNEL_AUTH_ERROR;
                log_tunnel_event(tunnel, "🔑 SSH authentication failed - check key and permissions");
            } else if (strstr(all_output, "bind: Address already in use") || 
                      strstr(all_output, "remote port forwarding failed") ||
                      strstr(all_output, "Warning: remote port forwarding failed") ||
                      strstr(all_output, "cannot listen to port") ||
                      strstr(all_output, "bind: Cannot assign requested address")) {
                tunnel->status = TUNNEL_PORT_ERROR;
                if (tunnel->type == TUNNEL_TYPE_REVERSE) {
                    log_tunnel_event(tunnel, "🔒 Remote port forwarding failed - check GatewayPorts setting and port availability on server");
                } else {
                    log_tunnel_event(tunnel, "🔒 Local port already in use - check for conflicting services");
                }
            } else {
                tunnel->status = TUNNEL_ERROR;
                log_tunnel_event(tunnel, "❌ SSH connection failed - check host, port, and network");
            }
            pthread_mutex_unlock(&manager.mutex);
            
        // If no immediate errors detected, check the process status
        if (!has_output || 
            !(strstr(all_output, "Permission denied") || strstr(all_output, "Connection refused") ||
              strstr(all_output, "remote port forwarding failed") || strstr(all_output, "bind:"))) {
            
            // Give SSH more time to establish, especially for reverse tunnels
            int wait_time = (tunnel->type == TUNNEL_TYPE_REVERSE) ? 5 : 2;
            sleep(wait_time);
            
            // Check if process is still alive
            if (ssh_proc) {
                // Try reading more output after waiting
                while (fgets(output_buffer, sizeof(output_buffer), ssh_proc)) {
                    output_buffer[strcspn(output_buffer, "\r\n")] = 0;
                    if (strlen(output_buffer) > 0) {
                        char log_msg[1024];
                        snprintf(log_msg, sizeof(log_msg), "🔍 Delayed SSH output: %s", output_buffer);
                        log_tunnel_event(tunnel, log_msg);
                        
                        // Check for delayed errors
                        if (strstr(output_buffer, "Warning: remote port forwarding failed") ||
                            strstr(output_buffer, "bind: Address already in use") ||
                            strstr(output_buffer, "bind: Cannot assign requested address")) {
                            pthread_mutex_lock(&manager.mutex);
                            tunnel->status = TUNNEL_PORT_ERROR;
                            log_tunnel_event(tunnel, "🔒 Delayed error: Remote port forwarding failed");
                            pthread_mutex_unlock(&manager.mutex);
                            
                            pclose(ssh_proc);
                            ssh_proc = NULL;
                            sleep(tunnel->reconnect_delay);
                            continue;
                        }
                    }
                }
            }
        } else {
            pclose(ssh_proc);
            ssh_proc = NULL;
            sleep(tunnel->reconnect_delay);
            continue;
        }
        
        pthread_mutex_lock(&manager.mutex);
        tunnel->status = TUNNEL_RUNNING;
        pthread_mutex_unlock(&manager.mutex);
        
        log_tunnel_event(tunnel, "✅ Tunnel established successfully");
        
        // Wait for process to exit (this blocks until SSH dies)
        int exit_code = pclose(ssh_proc);
        ssh_proc = NULL;
        
        // Check if SSH failed (key problems, auth issues, etc.)
        if (exit_code != 0) {
            pthread_mutex_lock(&manager.mutex);
            // Exit code 255 usually indicates SSH authentication/connection failure
            if (exit_code == 255) {
                tunnel->status = TUNNEL_AUTH_ERROR;
                log_tunnel_event(tunnel, "🔑 SSH authentication failed (check key, permissions, host access)");
            } else {
                tunnel->status = TUNNEL_ERROR;
                log_tunnel_event(tunnel, "❌ SSH process exited with error (check configuration)");
            }
            pthread_mutex_unlock(&manager.mutex);
            
            sleep(tunnel->reconnect_delay);
            continue;
        }
        
        pthread_mutex_lock(&manager.mutex);
        tunnel->status = tunnel->should_run ? TUNNEL_RECONNECTING : TUNNEL_STOPPED;
        pthread_mutex_unlock(&manager.mutex);
        
        if (tunnel->should_run) {
            log_tunnel_event(tunnel, "💔 Tunnel died, reconnecting...");
            sleep(tunnel->reconnect_delay);
        } else {
            log_tunnel_event(tunnel, "🛑 Tunnel stopped by user");
            break;
        }
    }
    
    pthread_mutex_lock(&manager.mutex);
    tunnel->status = TUNNEL_STOPPED;
    pthread_mutex_unlock(&manager.mutex);
    
    log_tunnel_event(tunnel, "👋 Tunnel worker thread exiting");
    return NULL;
}

int load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open config file '%s': %s\n", 
                filename, strerror(errno));
        return -1;
    }
    
    // Read file content
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char *json_string = malloc(file_size + 1);
    if (!json_string) {
        fclose(file);
        return -1;
    }
    
    fread(json_string, 1, file_size, file);
    json_string[file_size] = '\0';
    fclose(file);
    
    // Parse JSON
    cJSON *json = cJSON_Parse(json_string);
    free(json_string);
    
    if (!json) {
        fprintf(stderr, "Error: Invalid JSON in config file\n");
        return -1;
    }
    
    cJSON *tunnels_array = cJSON_GetObjectItem(json, "tunnels");
    if (!cJSON_IsArray(tunnels_array)) {
        fprintf(stderr, "Error: 'tunnels' must be an array\n");
        cJSON_Delete(json);
        return -1;
    }
    
    int tunnel_count = cJSON_GetArraySize(tunnels_array);
    if (tunnel_count > MAX_TUNNELS) {
        fprintf(stderr, "Error: Too many tunnels (max %d)\n", MAX_TUNNELS);
        cJSON_Delete(json);
        return -1;
    }
    
    manager.count = 0;
    
    for (int i = 0; i < tunnel_count; i++) {
        cJSON *tunnel_json = cJSON_GetArrayItem(tunnels_array, i);
        if (!cJSON_IsObject(tunnel_json)) continue;
        
        tunnel_t *tunnel = &manager.tunnels[manager.count];
        memset(tunnel, 0, sizeof(tunnel_t));
        
        // Parse tunnel configuration
        cJSON *name = cJSON_GetObjectItem(tunnel_json, "name");
        cJSON *host = cJSON_GetObjectItem(tunnel_json, "host");
        cJSON *port = cJSON_GetObjectItem(tunnel_json, "port");
        cJSON *user = cJSON_GetObjectItem(tunnel_json, "user");
        cJSON *ssh_key = cJSON_GetObjectItem(tunnel_json, "ssh_key");
        cJSON *type = cJSON_GetObjectItem(tunnel_json, "type");
        cJSON *local_port = cJSON_GetObjectItem(tunnel_json, "local_port");
        cJSON *remote_host = cJSON_GetObjectItem(tunnel_json, "remote_host");
        cJSON *remote_port = cJSON_GetObjectItem(tunnel_json, "remote_port");
        cJSON *reconnect_delay = cJSON_GetObjectItem(tunnel_json, "reconnect_delay");
        
        if (!cJSON_IsString(name) || !cJSON_IsString(host) || 
            !cJSON_IsNumber(port) || !cJSON_IsString(user) ||
            !cJSON_IsString(ssh_key) || !cJSON_IsNumber(local_port) || 
            !cJSON_IsString(remote_host) || !cJSON_IsNumber(remote_port)) {
            fprintf(stderr, "Error: Invalid tunnel configuration at index %d\n", i);
            continue;
        }
        
        strncpy(tunnel->name, cJSON_GetStringValue(name), MAX_NAME_LEN - 1);
        strncpy(tunnel->host, cJSON_GetStringValue(host), MAX_HOST_LEN - 1);
        tunnel->port = cJSON_GetNumberValue(port);
        strncpy(tunnel->user, cJSON_GetStringValue(user), MAX_NAME_LEN - 1);
        strncpy(tunnel->ssh_key, cJSON_GetStringValue(ssh_key), MAX_PATH_LEN - 1);
        
        // Parse tunnel type (default: forward)
        if (cJSON_IsString(type)) {
            const char *type_str = cJSON_GetStringValue(type);
            if (strcmp(type_str, "reverse") == 0) {
                tunnel->type = TUNNEL_TYPE_REVERSE;
            } else {
                tunnel->type = TUNNEL_TYPE_FORWARD;  // default
            }
        } else {
            tunnel->type = TUNNEL_TYPE_FORWARD;  // default if not specified
        }
        
        tunnel->local_port = cJSON_GetNumberValue(local_port);
        strncpy(tunnel->remote_host, cJSON_GetStringValue(remote_host), MAX_HOST_LEN - 1);
        tunnel->remote_port = cJSON_GetNumberValue(remote_port);
        tunnel->reconnect_delay = cJSON_IsNumber(reconnect_delay) ? 
                                 cJSON_GetNumberValue(reconnect_delay) : 5;
        
        // Validate SSH key at startup
        struct stat key_stat;
        if (stat(tunnel->ssh_key, &key_stat) != 0) {
            fprintf(stderr, "%s⚠️  Warning: SSH key '%s' for tunnel '%s' does not exist%s\n", 
                   C_WARNING, tunnel->ssh_key, tunnel->name, C_RESET);
        } else if ((key_stat.st_mode & 0777) > 0600) {
            fprintf(stderr, "%s⚠️  Warning: SSH key '%s' for tunnel '%s' has loose permissions (%o, should be 600)%s\n", 
                   C_WARNING, tunnel->ssh_key, tunnel->name, key_stat.st_mode & 0777, C_RESET);
        }
        
        // Open log file
        char log_path[256];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, tunnel->name);
        tunnel->log = fopen(log_path, "a");
        if (!tunnel->log) {
            fprintf(stderr, "%s⚠️  Warning: Cannot open log file '%s' for tunnel '%s': %s%s\n", 
                   C_WARNING, log_path, tunnel->name, strerror(errno), C_RESET);
        }
        
        tunnel->status = TUNNEL_STOPPED;
        tunnel->should_run = 0;
        
        manager.count++;
    }
    
    cJSON_Delete(json);
    printf("%s✅ Loaded %s%d%s tunnels from config%s\n", 
           C_SUCCESS, C_BOLD, manager.count, C_RESET, C_RESET);
    return 0;
}

void save_config(const char *filename) {
    cJSON *json = cJSON_CreateObject();
    cJSON *tunnels_arr = cJSON_CreateArray();
    
    pthread_mutex_lock(&manager.mutex);
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *t = &manager.tunnels[i];
        cJSON *tunnel_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(tunnel_obj, "name", t->name);
        cJSON_AddStringToObject(tunnel_obj, "user", t->user);
        cJSON_AddStringToObject(tunnel_obj, "host", t->host);
        cJSON_AddNumberToObject(tunnel_obj, "port", t->port);
        cJSON_AddStringToObject(tunnel_obj, "ssh_key", t->ssh_key);
        cJSON_AddStringToObject(tunnel_obj, "type", t->type == TUNNEL_TYPE_REVERSE ? "reverse" : "forward");
        cJSON_AddNumberToObject(tunnel_obj, "local_port", t->local_port);
        cJSON_AddStringToObject(tunnel_obj, "remote_host", t->remote_host);
        cJSON_AddNumberToObject(tunnel_obj, "remote_port", t->remote_port);
        cJSON_AddNumberToObject(tunnel_obj, "reconnect_delay", t->reconnect_delay);
        cJSON_AddItemToArray(tunnels_arr, tunnel_obj);
    }
    cJSON_AddItemToObject(json, "tunnels", tunnels_arr);
    pthread_mutex_unlock(&manager.mutex);

    char *json_string = cJSON_Print(json);
    if (json_string) {
        FILE *file = fopen(filename, "w");
        if (file) {
            fputs(json_string, file);
            fclose(file);
            printf("%s💾 Configuration saved to %s%s%s\n", C_SUCCESS, C_BOLD, filename, C_RESET);
        } else {
            fprintf(stderr, "%s❌ Failed to save config to %s%s\n", C_ERROR, filename, C_RESET);
        }
        free(json_string);
    }
    cJSON_Delete(json);
}
//}

void start_all_tunnels(void) {
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        tunnel->should_run = 1;
        if (pthread_create(&tunnel->thread, NULL, tunnel_worker, tunnel) != 0) {
            fprintf(stderr, "Error: Failed to create thread for tunnel '%s'\n", tunnel->name);
            tunnel->should_run = 0;
        }
    }
}

void stop_all_tunnels(void) {
    // Signal all tunnels to stop
    for (int i = 0; i < manager.count; i++) {
        manager.tunnels[i].should_run = 0;
    }
    
    // Wait for all threads to finish
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        if (tunnel->thread) {
            pthread_join(tunnel->thread, NULL);
            tunnel->thread = 0;
        }
    }
}

void start_tunnel_by_name(const char *name) {
    pthread_mutex_lock(&manager.mutex);
    int found = 0;
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        if (strcmp(tunnel->name, name) == 0) {
            if (!tunnel->should_run) {
                tunnel->should_run = 1;
                if (pthread_create(&tunnel->thread, NULL, tunnel_worker, tunnel) == 0) {
                    printf("%s🚀 Started tunnel '%s%s%s'%s\n", C_SUCCESS, C_BOLD, name, C_RESET, C_RESET);
                } else {
                    fprintf(stderr, "%s❌ Failed to create thread for tunnel '%s'%s\n", C_ERROR, name, C_RESET);
                    tunnel->should_run = 0;
                }
            } else {
                printf("%s⚠️  Tunnel '%s%s%s' is already running%s\n", C_WARNING, C_BOLD, name, C_RESET, C_RESET);
            }
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&manager.mutex);
    
    if (!found) {
        printf("%s❌ Tunnel '%s%s%s' not found%s\n", C_ERROR, C_BOLD, name, C_RESET, C_RESET);
    }
}

void stop_tunnel_by_name(const char *name) {
    pthread_mutex_lock(&manager.mutex);
    int found = 0;
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        if (strcmp(tunnel->name, name) == 0) {
            tunnel->should_run = 0;
            if (tunnel->thread) {
                pthread_mutex_unlock(&manager.mutex);  // Unlock before join
                pthread_join(tunnel->thread, NULL);
                pthread_mutex_lock(&manager.mutex);    // Re-lock
                tunnel->thread = 0;
            }
            printf("%s🛑 Stopped tunnel '%s%s%s'%s\n", C_WARNING, C_BOLD, name, C_RESET, C_RESET);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&manager.mutex);
    
    if (!found) {
        printf("%s❌ Tunnel '%s%s%s' not found%s\n", C_ERROR, C_BOLD, name, C_RESET, C_RESET);
    }
}

void reset_tunnel_by_name(const char *name) {
    pthread_mutex_lock(&manager.mutex);
    int found = 0;
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        if (strcmp(tunnel->name, name) == 0) {
            // Stop first
            tunnel->should_run = 0;
            if (tunnel->thread) {
                pthread_mutex_unlock(&manager.mutex);  // Unlock before join
                pthread_join(tunnel->thread, NULL);
                pthread_mutex_lock(&manager.mutex);    // Re-lock
                tunnel->thread = 0;
            }
            
            // Reset restart counter
            tunnel->restart_count = 0;
            
            // Start again
            tunnel->should_run = 1;
            if (pthread_create(&tunnel->thread, NULL, tunnel_worker, tunnel) == 0) {
                printf("%s🔄 Reset tunnel '%s%s%s'%s\n", C_INFO, C_BOLD, name, C_RESET, C_RESET);
            } else {
                fprintf(stderr, "%s❌ Failed to restart tunnel '%s'%s\n", C_ERROR, name, C_RESET);
                tunnel->should_run = 0;
            }
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&manager.mutex);
    
    if (!found) {
        printf("%s❌ Tunnel '%s%s%s' not found%s\n", C_ERROR, C_BOLD, name, C_RESET, C_RESET);
    }
}

void add_tunnel_interactive(void) {
    char name[MAX_NAME_LEN], user[MAX_NAME_LEN], host[MAX_HOST_LEN], remote_host[MAX_HOST_LEN];
    char ssh_key[MAX_PATH_LEN];
    char type_input[16];
    tunnel_type_t tunnel_type = TUNNEL_TYPE_FORWARD;  // default
    int port, local_port, remote_port, reconnect_delay = 5;
    char input_buffer[256];
    
    printf("\n%s📝 Adding new tunnel - Interactive Setup%s\n", C_BOLD, C_RESET);
    printf("%s─────────────────────────────────────────%s\n\n", C_GREY, C_RESET);
    
    // Check if we have space
    pthread_mutex_lock(&manager.mutex);
    if (manager.count >= MAX_TUNNELS) {
        printf("%s❌ Maximum tunnels reached (%d/%d)%s\n", C_ERROR, MAX_TUNNELS, MAX_TUNNELS, C_RESET);
        pthread_mutex_unlock(&manager.mutex);
        return;
    }
    pthread_mutex_unlock(&manager.mutex);
    
    printf("%sTunnel name:%s ", C_CYAN, C_RESET);
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0;
    
    printf("%sSSH user:%s ", C_CYAN, C_RESET);
    fgets(user, sizeof(user), stdin);
    user[strcspn(user, "\n")] = 0;
    
    printf("%sSSH host:%s ", C_CYAN, C_RESET);
    fgets(host, sizeof(host), stdin);
    host[strcspn(host, "\n")] = 0;
    
    printf("%sSSH port:%s ", C_CYAN, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    port = atoi(input_buffer);
    
    printf("%sSSH private key path:%s ", C_CYAN, C_RESET);
    fgets(ssh_key, sizeof(ssh_key), stdin);
    ssh_key[strcspn(ssh_key, "\n")] = 0;
    
    printf("\n%s📡 Tunnel Type Selection:%s\n", C_BOLD, C_RESET);
    printf("  %s[F]orward%s - Remote service accessible locally (ssh -L)\n", C_GREEN, C_RESET);
    printf("  %s[R]everse%s - Local service accessible remotely (ssh -R)\n", C_MAGENTA, C_RESET);
    printf("%sTunnel type [F/r]:%s ", C_CYAN, C_RESET);
    fgets(type_input, sizeof(type_input), stdin);
    type_input[strcspn(type_input, "\n")] = 0;
    
    if (type_input[0] == 'r' || type_input[0] == 'R') {
        tunnel_type = TUNNEL_TYPE_REVERSE;
        printf("%s✅ Selected: Reverse tunnel (local→remote)%s\n\n", C_MAGENTA, C_RESET);
    } else {
        tunnel_type = TUNNEL_TYPE_FORWARD;
        printf("%s✅ Selected: Forward tunnel (remote→local)%s\n\n", C_GREEN, C_RESET);
    }
    
    printf("%sLocal port:%s ", C_CYAN, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    local_port = atoi(input_buffer);
    
    if (tunnel_type == TUNNEL_TYPE_REVERSE) {
        printf("%sRemote port (will be opened on %s%s%s):%s ", C_CYAN, C_BOLD, host, C_RESET, C_RESET);
    } else {
        printf("%sRemote host:%s ", C_CYAN, C_RESET);
    }
    if (tunnel_type == TUNNEL_TYPE_REVERSE) {
        fgets(input_buffer, sizeof(input_buffer), stdin);
        remote_port = atoi(input_buffer);
        strcpy(remote_host, "127.0.0.1");  // For reverse tunnels, always localhost
    } else {
        fgets(remote_host, sizeof(remote_host), stdin);
        remote_host[strcspn(remote_host, "\n")] = 0;
        
        printf("%sRemote port:%s ", C_CYAN, C_RESET);
        fgets(input_buffer, sizeof(input_buffer), stdin);
        remote_port = atoi(input_buffer);
    }
    
    printf("%sReconnect delay (s) [%d]:%s ", C_CYAN, reconnect_delay, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    if (strlen(input_buffer) > 1) {
        reconnect_delay = atoi(input_buffer);
    }
    
    // Validate input
    if (strlen(name) == 0 || strlen(user) == 0 || strlen(host) == 0 || 
        strlen(ssh_key) == 0 || strlen(remote_host) == 0 || 
        port <= 0 || local_port <= 0 || remote_port <= 0) {
        printf("%s❌ Invalid input. Tunnel not added.%s\n", C_ERROR, C_RESET);
        return;
    }
    
    // Check if SSH key file exists and has proper permissions
    struct stat key_stat;
    if (stat(ssh_key, &key_stat) != 0) {
        printf("%s❌ SSH key file '%s' does not exist%s\n", C_ERROR, ssh_key, C_RESET);
        return;
    }
    
    // Check permissions (should be 600 or 400 for private keys)
    if ((key_stat.st_mode & 0777) > 0600) {
        printf("%s⚠️  Warning: SSH key '%s' has loose permissions (should be 600)%s\n", 
               C_WARNING, ssh_key, C_RESET);
        printf("%sFile permissions: %o (should be 600 for security)%s\n", 
               C_DIM, key_stat.st_mode & 0777, C_RESET);
        printf("%sContinue anyway? [y/N]:%s ", C_YELLOW, C_RESET);
        
        char confirm[16];
        fgets(confirm, sizeof(confirm), stdin);
        if (confirm[0] != 'y' && confirm[0] != 'Y') {
            printf("%s❌ Tunnel not added. Fix key permissions first: chmod 600 %s%s\n", 
                   C_ERROR, ssh_key, C_RESET);
            return;
        }
    }
    
    // Check for duplicate names
    pthread_mutex_lock(&manager.mutex);
    for (int i = 0; i < manager.count; i++) {
        if (strcmp(manager.tunnels[i].name, name) == 0) {
            printf("%s❌ Tunnel with name '%s' already exists%s\n", C_ERROR, name, C_RESET);
            pthread_mutex_unlock(&manager.mutex);
            return;
        }
    }
    
    // Add the tunnel
    tunnel_t *tunnel = &manager.tunnels[manager.count];
    memset(tunnel, 0, sizeof(tunnel_t));
    
    strncpy(tunnel->name, name, MAX_NAME_LEN - 1);
    strncpy(tunnel->user, user, MAX_NAME_LEN - 1);
    strncpy(tunnel->host, host, MAX_HOST_LEN - 1);
    tunnel->port = port;
    strncpy(tunnel->ssh_key, ssh_key, MAX_PATH_LEN - 1);
    tunnel->type = tunnel_type;
    tunnel->local_port = local_port;
    strncpy(tunnel->remote_host, remote_host, MAX_HOST_LEN - 1);
    tunnel->remote_port = remote_port;
    tunnel->reconnect_delay = reconnect_delay;
    tunnel->should_run = 0;
    tunnel->status = TUNNEL_STOPPED;
    
    // Open log file
    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, tunnel->name);
    tunnel->log = fopen(log_path, "a");
    if (!tunnel->log) {
        fprintf(stderr, "%s⚠️  Warning: Cannot open log file for tunnel '%s'%s\n", C_WARNING, tunnel->name, C_RESET);
    }
    
    manager.count++;
    pthread_mutex_unlock(&manager.mutex);
    
    // Save config
    save_config(CONFIG_FILE);
    
    printf("\n%s✅ Tunnel '%s%s%s' added successfully!%s\n", C_SUCCESS, C_BOLD, name, C_RESET, C_RESET);
    
    // Ask if they want to start it now
    printf("%sStart tunnel now? [y/N]:%s ", C_YELLOW, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    if (input_buffer[0] == 'y' || input_buffer[0] == 'Y') {
        start_tunnel_by_name(name);
    }
    printf("\n");
}

void print_status(void) {
    const char *status_strings[] = {
        C_GREY "STOPPED" C_RESET, 
        C_YELLOW "STARTING" C_RESET, 
        C_GREEN "RUNNING" C_RESET, 
        C_RED "ERROR" C_RESET, 
        C_MAGENTA "AUTH-ERROR" C_RESET,  // SSH key/auth problems
        C_RED "PORT-ERROR" C_RESET,      // Port already in use
        C_YELLOW "RECONNECTING" C_RESET
    };
    
    const char *status_symbols[] = {
        SYMBOL_STOPPED,
        SYMBOL_STARTING,
        SYMBOL_RUNNING,
        SYMBOL_ERROR,
        "🔑",  // Key symbol for auth errors
        "🔒",  // Lock symbol for port errors
        SYMBOL_RECONNECT
    };
    
    // Clear screen für live feeling
    #ifndef _WIN32
    system("clear");
    #else
    system("cls");
    #endif
    
    printf("%s╔══════════════════════════════════════════════════════════════════════════╗%s\n", C_CYAN, C_RESET);
    printf("%s║%s %sChief Tunnel Officer - SSH Tunnel Manager v1.0%s %s║%s\n", 
           C_CYAN, C_RESET, C_BOLD, C_RESET, C_CYAN, C_RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════╝%s\n\n", C_CYAN, C_RESET);
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    printf("%sLive Status%s [%s%s%s] | Tunnels: %s%d%s\n\n", 
           C_BOLD, C_RESET, C_DIM, timestamp, C_RESET, C_BOLD, manager.count, C_RESET);
    
    pthread_mutex_lock(&manager.mutex);
    
    int running_count = 0;
    int error_count = 0;
    int auth_error_count = 0;
    int port_error_count = 0;
    
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        
        // Count status
        if (tunnel->status == TUNNEL_RUNNING) running_count++;
        if (tunnel->status == TUNNEL_ERROR) error_count++;
        if (tunnel->status == TUNNEL_AUTH_ERROR) auth_error_count++;
        if (tunnel->status == TUNNEL_PORT_ERROR) port_error_count++;
        
        // Status symbol mit Farbe
        printf("%s %s%s%s ", 
               status_symbols[tunnel->status],
               C_BOLD, tunnel->name, C_RESET);
               
        // Tunnel type indicator
        const char *type_symbol = (tunnel->type == TUNNEL_TYPE_REVERSE) ? "⬅️" : "➡️";
        const char *type_text = (tunnel->type == TUNNEL_TYPE_REVERSE) ? "REVERSE" : "FORWARD";
        
        // Connection info
        if (tunnel->type == TUNNEL_TYPE_REVERSE) {
            // Reverse: Local service accessible remotely
            printf("%s%s%s@%s%s%s:%s%d%s %s%s%s %s%s%s:%s%d%s %s%s%s localhost:%s%d%s %s[%s]%s\n",
                   C_DIM, tunnel->user, C_RESET,
                   C_BLUE, tunnel->host, C_RESET,
                   C_DIM, tunnel->port, C_RESET,
                   C_YELLOW, SYMBOL_ARROW, C_RESET,
                   C_GREEN, tunnel->host, C_RESET,
                   C_GREEN, tunnel->remote_port, C_RESET,
                   C_YELLOW, SYMBOL_ARROW, C_RESET,
                   C_BLUE, tunnel->local_port, C_RESET,
                   C_DIM, type_text, C_RESET);
        } else {
            // Forward: Remote service accessible locally
            printf("%s%s%s@%s%s%s:%s%d%s %s%s%s localhost:%s%d%s %s%s%s %s%s%s:%s%d%s %s[%s]%s\n",
                   C_DIM, tunnel->user, C_RESET,
                   C_BLUE, tunnel->host, C_RESET,
                   C_DIM, tunnel->port, C_RESET,
                   C_YELLOW, SYMBOL_ARROW, C_RESET,
                   C_GREEN, tunnel->local_port, C_RESET,
                   C_YELLOW, SYMBOL_ARROW, C_RESET,
                   C_BLUE, tunnel->remote_host, C_RESET,
                   C_DIM, tunnel->remote_port, C_RESET,
                   C_DIM, type_text, C_RESET);
        }
               
        // Status details mit schöner Formatierung
        printf("   Status: %s | Restarts: %s%d%s | Delay: %s%ds%s",
               status_strings[tunnel->status], 
               C_CYAN, tunnel->restart_count, C_RESET,
               C_DIM, tunnel->reconnect_delay, C_RESET);
               
        if (tunnel->last_restart > 0) {
            time_t diff = now - tunnel->last_restart;
            printf(" | Last: %s%lds ago%s", C_DIM, diff, C_RESET);
        }
        printf("\n\n");
    }
    
    pthread_mutex_unlock(&manager.mutex);
    
    // Summary bar
    printf("%s┌─ Summary ──────────────────────────────────────────────────────────────────┐%s\n", C_GREY, C_RESET);
    printf("%s│%s %sRunning:%s %s%d%s  %sErrors:%s %s%d%s  %sAuth:%s %s%d%s  %sPort:%s %s%d%s  %sTotal:%s %s%d%s %s│%s\n", 
           C_GREY, C_RESET,
           C_SUCCESS, C_RESET, C_BOLD, running_count, C_RESET,
           C_ERROR, C_RESET, C_BOLD, error_count, C_RESET,
           C_MAGENTA, C_RESET, C_BOLD, auth_error_count, C_RESET,
           C_RED, C_RESET, C_BOLD, port_error_count, C_RESET,
           C_INFO, C_RESET, C_BOLD, manager.count, C_RESET,
           C_GREY, C_RESET);
    printf("%s└────────────────────────────────────────────────────────────────────────────┘%s\n\n", C_GREY, C_RESET);
}

void interactive_mode(void) {
    char input[256];
    
    // Zeige initial den Status
    print_status();
    
    printf("%s=== Interactive Command Mode ===%s\n", C_BOLD, C_RESET);
    printf("Commands: %sstatus%s, %sstart%s [name], %sstop%s [name], %sreset%s <name>, %sadd%s, %stest%s [name], %sdebug%s [name], %sdiagnose%s, %swatch%s, %squit%s, %shelp%s\n\n", 
           C_CYAN, C_RESET, C_GREEN, C_RESET, C_RED, C_RESET, 
           C_MAGENTA, C_RESET, C_BLUE, C_RESET, C_YELLOW, C_RESET,
           C_RED, C_RESET, C_CYAN, C_RESET, C_YELLOW, C_RESET, C_MAGENTA, C_RESET, C_BLUE, C_RESET);
    
    while (manager.running) {
        printf("%stunnel%s> ", C_BOLD, C_RESET);
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        if (strcmp(input, "status") == 0 || strcmp(input, "") == 0) {
            print_status();
            printf("\n");
        } else if (strcmp(input, "start") == 0) {
            printf("%s⚡ Starting all tunnels...%s\n", C_YELLOW, C_RESET);
            start_all_tunnels();
            printf("%s✅ All tunnels started%s\n\n", C_SUCCESS, C_RESET);
        } else if (strncmp(input, "start ", 6) == 0) {
            char *name = input + 6;
            while (*name == ' ') name++; // Skip leading spaces
            if (strlen(name) > 0) {
                start_tunnel_by_name(name);
            } else {
                printf("%s❌ Usage: start <tunnel_name>%s\n", C_ERROR, C_RESET);
            }
        } else if (strcmp(input, "stop") == 0) {
            printf("%s🛑 Stopping all tunnels...%s\n", C_ERROR, C_RESET);
            stop_all_tunnels();
            printf("%s✅ All tunnels stopped%s\n\n", C_SUCCESS, C_RESET);
        } else if (strncmp(input, "stop ", 5) == 0) {
            char *name = input + 5;
            while (*name == ' ') name++; // Skip leading spaces
            if (strlen(name) > 0) {
                stop_tunnel_by_name(name);
            } else {
                printf("%s❌ Usage: stop <tunnel_name>%s\n", C_ERROR, C_RESET);
            }
        } else if (strncmp(input, "reset ", 6) == 0) {
            char *name = input + 6;
            while (*name == ' ') name++; // Skip leading spaces
            if (strlen(name) > 0) {
                reset_tunnel_by_name(name);
            } else {
                printf("%s❌ Usage: reset <tunnel_name>%s\n", C_ERROR, C_RESET);
            }
        } else if (strcmp(input, "add") == 0) {
            add_tunnel_interactive();
        } else if (strcmp(input, "test") == 0) {
            printf("%s🔧 Testing all tunnel connectivity...%s\n", C_INFO, C_RESET);
            pthread_mutex_lock(&manager.mutex);
            for (int i = 0; i < manager.count; i++) {
                tunnel_t *tunnel = &manager.tunnels[i];
                if (tunnel->status == TUNNEL_RUNNING) {
                    if (test_tunnel_connectivity(tunnel)) {
                        printf("%s✅ Tunnel '%s' is working (port %d accessible)%s\n", 
                               C_SUCCESS, tunnel->name, tunnel->local_port, C_RESET);
                    } else {
                        printf("%s❌ Tunnel '%s' appears broken (port %d not accessible)%s\n", 
                               C_ERROR, tunnel->name, tunnel->local_port, C_RESET);
                    }
                } else {
                    printf("%s⚠️  Tunnel '%s' is not running%s\n", C_WARNING, tunnel->name, C_RESET);
                }
            }
            pthread_mutex_unlock(&manager.mutex);
        } else if (strncmp(input, "test ", 5) == 0) {
            char *name = input + 5;
            while (*name == ' ') name++; // Skip leading spaces
            if (strlen(name) > 0) {
                pthread_mutex_lock(&manager.mutex);
                int found = 0;
                for (int i = 0; i < manager.count; i++) {
                    tunnel_t *tunnel = &manager.tunnels[i];
                    if (strcmp(tunnel->name, name) == 0) {
                        found = 1;
                        if (tunnel->status == TUNNEL_RUNNING) {
                            if (test_tunnel_connectivity(tunnel)) {
                                printf("%s✅ Tunnel '%s' is working (port %d accessible)%s\n", 
                                       C_SUCCESS, tunnel->name, tunnel->local_port, C_RESET);
                            } else {
                                printf("%s❌ Tunnel '%s' appears broken (port %d not accessible)%s\n", 
                                       C_ERROR, tunnel->name, tunnel->local_port, C_RESET);
                            }
                        } else {
                            printf("%s⚠️  Tunnel '%s' is not running (status: %s)%s\n", 
                                   C_WARNING, tunnel->name, 
                                   tunnel->status == TUNNEL_AUTH_ERROR ? "AUTH-ERROR" :
                                   tunnel->status == TUNNEL_PORT_ERROR ? "PORT-ERROR" :
                                   tunnel->status == TUNNEL_ERROR ? "ERROR" : "STOPPED", C_RESET);
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&manager.mutex);
                
                if (!found) {
                    printf("%s❌ Tunnel '%s' not found%s\n", C_ERROR, name, C_RESET);
                }
            } else {
                printf("%s❌ Usage: test <tunnel_name>%s\n", C_ERROR, C_RESET);
            }
        } else if (strcmp(input, "debug") == 0) {
            printf("%s🐛 Debug: Testing SSH commands for all tunnels%s\n", C_WARNING, C_RESET);
            pthread_mutex_lock(&manager.mutex);
            for (int i = 0; i < manager.count; i++) {
                tunnel_t *tunnel = &manager.tunnels[i];
                char cmd[MAX_CMD_LEN];
                
                printf("\n%s%s [%s]:%s\n", C_CYAN, tunnel->name, 
                       tunnel->type == TUNNEL_TYPE_REVERSE ? "REVERSE" : "FORWARD", C_RESET);
                
                if (tunnel->type == TUNNEL_TYPE_REVERSE) {
                    snprintf(cmd, sizeof(cmd),
                            "ssh -i %s -N -R %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no",
                            tunnel->ssh_key, tunnel->remote_port, tunnel->remote_host, tunnel->local_port,
                            tunnel->user, tunnel->host, tunnel->port);
                } else {
                    snprintf(cmd, sizeof(cmd),
                            "ssh -i %s -N -L %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no",
                            tunnel->ssh_key, tunnel->local_port, tunnel->remote_host, tunnel->remote_port,
                            tunnel->user, tunnel->host, tunnel->port);
                }
                
                printf("%s📝 SSH Command:%s\n%s%s%s\n", C_DIM, C_RESET, C_YELLOW, cmd, C_RESET);
            }
            pthread_mutex_unlock(&manager.mutex);
        } else if (strncmp(input, "debug ", 6) == 0) {
            char *name = input + 6;
            while (*name == ' ') name++;
            if (strlen(name) > 0) {
                pthread_mutex_lock(&manager.mutex);
                int found = 0;
                for (int i = 0; i < manager.count; i++) {
                    tunnel_t *tunnel = &manager.tunnels[i];
                    if (strcmp(tunnel->name, name) == 0) {
                        found = 1;
                        char cmd[MAX_CMD_LEN];
                        
                        printf("%s🐛 Debug: SSH command for %s [%s]%s\n", C_WARNING, tunnel->name,
                               tunnel->type == TUNNEL_TYPE_REVERSE ? "REVERSE" : "FORWARD", C_RESET);
                        
                        if (tunnel->type == TUNNEL_TYPE_REVERSE) {
                            snprintf(cmd, sizeof(cmd),
                                    "ssh -i %s -N -R %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no",
                                    tunnel->ssh_key, tunnel->remote_port, tunnel->remote_host, tunnel->local_port,
                                    tunnel->user, tunnel->host, tunnel->port);
                        } else {
                            snprintf(cmd, sizeof(cmd),
                                    "ssh -i %s -N -L %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no",
                                    tunnel->ssh_key, tunnel->local_port, tunnel->remote_host, tunnel->remote_port,
                                    tunnel->user, tunnel->host, tunnel->port);
                        }
                        
                        printf("%s📝 SSH Command:%s\n%s%s%s\n", C_DIM, C_RESET, C_YELLOW, cmd, C_RESET);
                        printf("%s💡 Manual test: Copy and run this command to debug manually%s\n", C_INFO, C_RESET);
                        break;
                    }
                }
                pthread_mutex_unlock(&manager.mutex);
                
                if (!found) {
                    printf("%s❌ Tunnel '%s' not found%s\n", C_ERROR, name, C_RESET);
                }
            } else {
                printf("%s❌ Usage: debug <tunnel_name>%s\n", C_ERROR, C_RESET);
            }
        } else if (strcmp(input, "diagnose") == 0) {
            printf("%s🔧 System Diagnostics%s\n\n", C_BOLD, C_RESET);
            
            // Check logs directory
            struct stat logs_stat;
            if (stat(LOG_DIR, &logs_stat) == 0) {
                printf("%s✅ Logs directory '%s' exists and is accessible%s\n", C_SUCCESS, LOG_DIR, C_RESET);
            } else {
                printf("%s❌ Logs directory '%s' is not accessible: %s%s\n", C_ERROR, LOG_DIR, strerror(errno), C_RESET);
            }
            
            // Check config file
            struct stat config_stat;
            if (stat(CONFIG_FILE, &config_stat) == 0) {
                printf("%s✅ Config file '%s' exists and is readable%s\n", C_SUCCESS, CONFIG_FILE, C_RESET);
            } else {
                printf("%s❌ Config file '%s' is not accessible: %s%s\n", C_ERROR, CONFIG_FILE, strerror(errno), C_RESET);
            }
            
            // Count tunnel types
            int reverse_count = 0, forward_count = 0;
            pthread_mutex_lock(&manager.mutex);
            for (int i = 0; i < manager.count; i++) {
                if (manager.tunnels[i].type == TUNNEL_TYPE_REVERSE) reverse_count++;
                else forward_count++;
            }
            pthread_mutex_unlock(&manager.mutex);
            
            printf("\n%sTunnel Type Distribution:%s\n", C_BOLD, C_RESET);
            printf("  %sForward tunnels (-L):%s %d\n", C_GREEN, C_RESET, forward_count);
            printf("  %sReverse tunnels (-R):%s %d\n", C_MAGENTA, C_RESET, reverse_count);
            
            if (reverse_count > 0) {
                printf("\n%s⚠️  Reverse Tunnel Requirements:%s\n", C_WARNING, C_RESET);
                printf("  • Remote server must have '%sGatewayPorts yes%s' in /etc/ssh/sshd_config\n", C_YELLOW, C_RESET);
                printf("  • Remote ports must be available (not in use)\n");
                printf("  • Firewall must allow the remote ports\n");
                printf("  • Use '%ssudo systemctl reload sshd%s' after config changes\n", C_YELLOW, C_RESET);
            }
            
            // Check all SSH keys
            printf("\n%sTunnel SSH Key Status:%s\n", C_BOLD, C_RESET);
            pthread_mutex_lock(&manager.mutex);
            for (int i = 0; i < manager.count; i++) {
                tunnel_t *tunnel = &manager.tunnels[i];
                struct stat key_stat;
                
                printf("  %s%s%s [%s]: ", C_CYAN, tunnel->name, C_RESET, 
                       tunnel->type == TUNNEL_TYPE_REVERSE ? "REVERSE" : "FORWARD");
                if (stat(tunnel->ssh_key, &key_stat) == 0) {
                    int perms = key_stat.st_mode & 0777;
                    if (perms <= 0600) {
                        printf("%s✅ Key exists, permissions OK (%o)%s\n", C_SUCCESS, perms, C_RESET);
                    } else {
                        printf("%s⚠️  Key exists but permissions too open (%o, should be 600)%s\n", C_WARNING, perms, C_RESET);
                    }
                } else {
                    printf("%s❌ Key not found: %s%s\n", C_ERROR, tunnel->ssh_key, C_RESET);
                }
            }
            pthread_mutex_unlock(&manager.mutex);
            printf("\n");
        } else if (strcmp(input, "watch") == 0) {
            printf("%s🔄 Entering watch mode (press Ctrl+C to exit)...%s\n\n", C_INFO, C_RESET);
            while (manager.running) {
                print_status();
                printf("%sRefreshing in 2 seconds... (Ctrl+C to exit watch mode)%s\n", C_DIM, C_RESET);
                sleep(2);
            }
        } else if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("%s👋 Chief Tunnel Officer signing off...%s\n", C_INFO, C_RESET);
            break;
        } else if (strcmp(input, "help") == 0) {
            printf("\n%s📋 Available Commands:%s\n", C_BOLD, C_RESET);
            printf("  %sstatus%s       - Show tunnel status (default)\n", C_CYAN, C_RESET);
            printf("  %sstart%s        - Start all tunnels\n", C_GREEN, C_RESET);
            printf("  %sstart <name>%s - Start specific tunnel\n", C_GREEN, C_RESET);
            printf("  %sstop%s         - Stop all tunnels\n", C_RED, C_RESET);
            printf("  %sstop <name>%s  - Stop specific tunnel\n", C_RED, C_RESET);
            printf("  %sreset <name>%s - Restart specific tunnel\n", C_MAGENTA, C_RESET);
            printf("  %sadd%s          - Add new tunnel interactively\n", C_BLUE, C_RESET);
            printf("  %stest%s         - Test all tunnel connectivity\n", C_YELLOW, C_RESET);
            printf("  %stest <name>%s  - Test specific tunnel connectivity\n", C_YELLOW, C_RESET);
            printf("  %sdebug%s        - Show SSH commands for all tunnels\n", C_RED, C_RESET);
            printf("  %sdebug <name>%s - Show SSH command for specific tunnel\n", C_RED, C_RESET);
            printf("  %sdiagnose%s     - Run system diagnostics\n", C_CYAN, C_RESET);
            printf("  %swatch%s        - Live status updates (refresh every 2s)\n", C_YELLOW, C_RESET);
            printf("  %squit%s         - Exit program\n", C_MAGENTA, C_RESET);
            printf("  %shelp%s         - Show this help\n\n", C_BLUE, C_RESET);
            printf("%s💡 Examples:%s\n", C_BOLD, C_RESET);
            printf("  start db-prod   %s# Start specific tunnel%s\n", C_DIM, C_RESET);
            printf("  stop web-dev    %s# Stop specific tunnel%s\n", C_DIM, C_RESET);
            printf("  test db-prod    %s# Test if tunnel is really working%s\n", C_DIM, C_RESET);
            printf("  diagnose        %s# Check system health and SSH keys%s\n", C_DIM, C_RESET);
            printf("  reset api-test  %s# Restart tunnel with reset counter%s\n\n", C_DIM, C_RESET);
            
            printf("%s🔄 Tunnel Types:%s\n", C_BOLD, C_RESET);
            printf("  %sForward (-L):%s Remote service → Local access\n", C_GREEN, C_RESET);
            printf("  %sReverse (-R):%s Local service → Remote access\n", C_MAGENTA, C_RESET);
            printf("  %sExample:%s ssh -R 6983:127.0.0.1:2283 user@server\n", C_DIM, C_RESET);
            printf("           %s# Opens port 6983 on server, forwards to local 2283%s\n\n", C_DIM, C_RESET);
        } else if (strlen(input) > 0) {
            printf("%s❌ Unknown command: %s%s%s (type '%shelp%s' for commands)%s\n\n", 
                   C_ERROR, C_BOLD, input, C_RESET, C_BLUE, C_RESET, C_RESET);
        }
    }
}

void signal_handler(int sig) {
    printf("\n%s🛑 Received signal %d, shutting down gracefully...%s\n", C_WARNING, sig, C_RESET);
    manager.running = 0;
}

void cleanup_manager(void) {
    stop_all_tunnels();
    
    // Close log files
    for (int i = 0; i < manager.count; i++) {
        if (manager.tunnels[i].log) {
            fclose(manager.tunnels[i].log);
            manager.tunnels[i].log = NULL;
        }
    }
    
    pthread_mutex_destroy(&manager.mutex);
}

int main(int argc, char **argv) {
    const char *config_file = (argc > 1) ? argv[1] : CONFIG_FILE;
    
    // Startup Banner
    printf("%s╔══════════════════════════════════════════════════════════════════════════╗%s\n", C_CYAN, C_RESET);
    printf("%s║%s %sChief Tunnel Officer - SSH Tunnel Manager v1.0%s %s║%s\n", 
           C_CYAN, C_RESET, C_BOLD, C_RESET, C_CYAN, C_RESET);
    printf("%s║%s %sThe ultimate SSH tunnel daemon for real engineers%s %s║%s\n", 
           C_CYAN, C_RESET, C_DIM, C_RESET, C_CYAN, C_RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════╝%s\n\n", C_CYAN, C_RESET);
    
    // Initialize
    manager.running = 1;
    if (pthread_mutex_init(&manager.mutex, NULL) != 0) {
        fprintf(stderr, "%s❌ Error: Failed to initialize mutex%s\n", C_ERROR, C_RESET);
        return 1;
    }
    
    // Create logs directory
    mkdir(LOG_DIR, 0755);
    printf("%s📁 Logs directory: %s%s%s\n", C_INFO, C_BOLD, LOG_DIR, C_RESET);
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    printf("%s⚡ Signal handlers registered%s\n", C_SUCCESS, C_RESET);
    
    // Load configuration
    printf("%s📋 Loading configuration from: %s%s%s\n", C_INFO, C_BOLD, config_file, C_RESET);
    if (load_config(config_file) != 0) {
        fprintf(stderr, "%s❌ Failed to load configuration%s\n", C_ERROR, C_RESET);
        cleanup_manager();
        return 1;
    }
    
    if (manager.count == 0) {
        printf("%s⚠️  No tunnels configured, exiting.%s\n", C_WARNING, C_RESET);
        cleanup_manager();
        return 1;
    }
    
    printf("%s✅ Loaded %s%d%s tunnels successfully%s\n\n", 
           C_SUCCESS, C_BOLD, manager.count, C_RESET, C_RESET);
    
    // Start tunnels
    printf("%s🚀 Auto-starting all tunnels...%s\n", C_INFO, C_RESET);
    start_all_tunnels();
    sleep(1); // Brief pause für startup
    
    // Enter interactive mode
    interactive_mode();
    
    // Cleanup
    printf("\n%s🛑 Initiating shutdown sequence...%s\n", C_WARNING, C_RESET);
    manager.running = 0;
    cleanup_manager();
    
    printf("%s👋 Chief Tunnel Officer signing off. All tunnels terminated.%s\n", C_SUCCESS, C_RESET);
    printf("%s══════════════════════════════════════════════════════════════════════════%s\n", C_GREY, C_RESET);
    return 0;
}
