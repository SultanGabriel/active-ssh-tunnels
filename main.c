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
#define sleep(x) Sleep((x) * 1000)
#define popen _popen
#define pclose _pclose
#define mkdir(path, mode) _mkdir(path)
#else
#include <ncurses.h>
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
    TUNNEL_RECONNECTING
} tunnel_status_t;

typedef struct {
    char name[MAX_NAME_LEN];
    char host[MAX_HOST_LEN];
    int port;
    char user[MAX_NAME_LEN];
    char ssh_key[MAX_PATH_LEN];  // SSH private key file path
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

void *tunnel_worker(void *arg) {
    tunnel_t *tunnel = (tunnel_t *)arg;
    char cmd[MAX_CMD_LEN];
    FILE *ssh_proc = NULL;
    
    while (tunnel->should_run && manager.running) {
        pthread_mutex_lock(&manager.mutex);
        tunnel->status = TUNNEL_STARTING;
        tunnel->restart_count++;
        tunnel->last_restart = time(NULL);
        pthread_mutex_unlock(&manager.mutex);
        
        log_tunnel_event(tunnel, "🚀 Starting SSH tunnel");
        
        // Build SSH command with SSH key authentication
        snprintf(cmd, sizeof(cmd),
                "ssh -i %s -N -L %d:%s:%d %s@%s -p %d -o ConnectTimeout=10 -o ServerAliveInterval=30 -o IdentitiesOnly=yes",
                tunnel->ssh_key, tunnel->local_port, tunnel->remote_host, tunnel->remote_port,
                tunnel->user, tunnel->host, tunnel->port);
        
        log_tunnel_event(tunnel, "📡 Executing SSH command");
        
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
        
        pthread_mutex_lock(&manager.mutex);
        tunnel->status = TUNNEL_RUNNING;
        pthread_mutex_unlock(&manager.mutex);
        
        log_tunnel_event(tunnel, "✅ Tunnel established successfully");
        
        // Wait for process to exit
        int exit_code = pclose(ssh_proc);
        ssh_proc = NULL;
        
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
        tunnel->local_port = cJSON_GetNumberValue(local_port);
        strncpy(tunnel->remote_host, cJSON_GetStringValue(remote_host), MAX_HOST_LEN - 1);
        tunnel->remote_port = cJSON_GetNumberValue(remote_port);
        tunnel->reconnect_delay = cJSON_IsNumber(reconnect_delay) ? 
                                 cJSON_GetNumberValue(reconnect_delay) : 5;
        
        // Open log file
        char log_path[256];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, tunnel->name);
        tunnel->log = fopen(log_path, "a");
        if (!tunnel->log) {
            fprintf(stderr, "Warning: Cannot open log file for tunnel '%s'\n", tunnel->name);
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
    
    printf("%sLocal port:%s ", C_CYAN, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    local_port = atoi(input_buffer);
    
    printf("%sRemote host:%s ", C_CYAN, C_RESET);
    fgets(remote_host, sizeof(remote_host), stdin);
    remote_host[strcspn(remote_host, "\n")] = 0;
    
    printf("%sRemote port:%s ", C_CYAN, C_RESET);
    fgets(input_buffer, sizeof(input_buffer), stdin);
    remote_port = atoi(input_buffer);
    
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
        C_YELLOW "RECONNECTING" C_RESET
    };
    
    const char *status_symbols[] = {
        SYMBOL_STOPPED,
        SYMBOL_STARTING,
        SYMBOL_RUNNING,
        SYMBOL_ERROR,
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
    
    for (int i = 0; i < manager.count; i++) {
        tunnel_t *tunnel = &manager.tunnels[i];
        
        // Count status
        if (tunnel->status == TUNNEL_RUNNING) running_count++;
        if (tunnel->status == TUNNEL_ERROR) error_count++;
        
        // Status symbol mit Farbe
        printf("%s %s%s%s ", 
               status_symbols[tunnel->status],
               C_BOLD, tunnel->name, C_RESET);
               
        // Connection info
        printf("%s%s%s@%s%s%s:%s%d%s %s%s%s localhost:%s%d%s %s%s%s %s%s%s:%s%d%s\n",
               C_DIM, tunnel->user, C_RESET,
               C_BLUE, tunnel->host, C_RESET,
               C_DIM, tunnel->port, C_RESET,
               C_YELLOW, SYMBOL_ARROW, C_RESET,
               C_GREEN, tunnel->local_port, C_RESET,
               C_YELLOW, SYMBOL_ARROW, C_RESET,
               C_BLUE, tunnel->remote_host, C_RESET,
               C_DIM, tunnel->remote_port, C_RESET);
               
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
    printf("%s│%s %sRunning:%s %s%d%s  %sErrors:%s %s%d%s  %sTotal:%s %s%d%s tunnels %s│%s\n", 
           C_GREY, C_RESET,
           C_SUCCESS, C_RESET, C_BOLD, running_count, C_RESET,
           C_ERROR, C_RESET, C_BOLD, error_count, C_RESET,
           C_INFO, C_RESET, C_BOLD, manager.count, C_RESET,
           C_GREY, C_RESET);
    printf("%s└────────────────────────────────────────────────────────────────────────────┘%s\n\n", C_GREY, C_RESET);
}

void interactive_mode(void) {
    char input[256];
    
    // Zeige initial den Status
    print_status();
    
    printf("%s=== Interactive Command Mode ===%s\n", C_BOLD, C_RESET);
    printf("Commands: %sstatus%s, %sstart%s [name], %sstop%s [name], %sreset%s <name>, %sadd%s, %swatch%s, %squit%s, %shelp%s\n\n", 
           C_CYAN, C_RESET, C_GREEN, C_RESET, C_RED, C_RESET, 
           C_MAGENTA, C_RESET, C_BLUE, C_RESET, C_YELLOW, C_RESET, 
           C_MAGENTA, C_RESET, C_BLUE, C_RESET);
    
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
            printf("  %swatch%s        - Live status updates (refresh every 2s)\n", C_YELLOW, C_RESET);
            printf("  %squit%s         - Exit program\n", C_MAGENTA, C_RESET);
            printf("  %shelp%s         - Show this help\n\n", C_BLUE, C_RESET);
            printf("%s💡 Examples:%s\n", C_BOLD, C_RESET);
            printf("  start db-prod   %s# Start specific tunnel%s\n", C_DIM, C_RESET);
            printf("  stop web-dev    %s# Stop specific tunnel%s\n", C_DIM, C_RESET);
            printf("  reset api-test  %s# Restart tunnel with reset counter%s\n\n", C_DIM, C_RESET);
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
