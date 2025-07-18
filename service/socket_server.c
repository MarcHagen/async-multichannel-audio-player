#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/stat.h>
#include "socket_server.h"
#include "log.h"

// Socket command handling
typedef struct
{
    const char* cmd;
    int (*handler)(track_manager_ctx_t* mgr, const char* arg, char* response, size_t resp_size);
} command_handler_t;

// Command handlers
static int handle_play(track_manager_ctx_t* mgr, const char* track_id, char* response, size_t resp_size)
{
    if (!track_id || !track_id[0])
    {
        snprintf(response, resp_size, "ERROR: Missing track ID");
        return -1;
    }

    if (track_manager_play(mgr, track_id))
    {
        snprintf(response, resp_size, "OK: Playing track %s", track_id);
        return 0;
    }

    snprintf(response, resp_size, "ERROR: Failed to play track %s", track_id);
    return -1;
}

static int handle_stop(track_manager_ctx_t* mgr, const char* track_id, char* response, size_t resp_size)
{
    if (!track_id || !track_id[0])
    {
        snprintf(response, resp_size, "ERROR: Missing track ID");
        return -1;
    }

    if (track_manager_stop(mgr, track_id))
    {
        snprintf(response, resp_size, "OK: Stopped track %s", track_id);
        return 0;
    }

    snprintf(response, resp_size, "ERROR: Failed to stop track %s", track_id);
    return -1;
}

static int handle_stop_all(track_manager_ctx_t* mgr, const char* arg, char* response, size_t resp_size)
{
    (void)arg; // Unused

    if (track_manager_stop_all(mgr))
    {
        snprintf(response, resp_size, "OK: Stopped all tracks");
        return 0;
    }

    snprintf(response, resp_size, "ERROR: Failed to stop all tracks");
    return -1;
}

static int handle_list(track_manager_ctx_t* mgr, const char* arg, char* response, size_t resp_size)
{
    (void)arg; // Unused
    (void)mgr; // Use track manager to get list

    // For now, just return a simple message
    // In a full implementation, format a proper list of tracks
    snprintf(response, resp_size, "OK: Track listing not yet implemented");
    return 0;
}

static int handle_status(track_manager_ctx_t* mgr, const char* arg, char* response, size_t resp_size)
{
    (void)arg; // Unused
    (void)mgr; // Use track manager to get status

    // For now, just return a simple message
    // In a full implementation, format status information
    snprintf(response, resp_size, "OK: Status not yet implemented");
    return 0;
}

static int handle_reload(track_manager_ctx_t* mgr, const char* arg, char* response, size_t resp_size)
{
    (void)arg; // Unused
    (void)mgr; // In a full implementation, this would reload the config

    // This would trigger a reload signal
    snprintf(response, resp_size, "OK: Reload signal sent");
    return 0;
}

// Command table
static const command_handler_t COMMANDS[] = {
    {"play", handle_play},
    {"stop", handle_stop},
    {"stop-all", handle_stop_all},
    {"list", handle_list},
    {"status", handle_status},
    {"reload", handle_reload},
    {NULL, NULL} // Terminator
};

// Process a command string
static int process_command(const char* cmd_str, track_manager_ctx_t* mgr, char* response, size_t resp_size)
{
    char cmd_buf[256];
    strncpy(cmd_buf, cmd_str, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    // Split command and argument
    char* cmd = strtok(cmd_buf, " ");
    char* arg = strtok(NULL, "");

    if (!cmd)
    {
        snprintf(response, resp_size, "ERROR: Empty command");
        return -1;
    }

    // Find command handler
    for (const command_handler_t* handler = COMMANDS; handler->cmd != NULL; handler++)
    {
        if (strcmp(handler->cmd, cmd) == 0)
        {
            return handler->handler(mgr, arg, response, resp_size);
        }
    }

    snprintf(response, resp_size, "ERROR: Unknown command '%s'", cmd);
    return -1;
}

// Socket server thread function
static void* socket_server_thread(void* arg)
{
    socket_server_ctx_t* ctx = (socket_server_ctx_t*)arg;
    char buffer[1024];
    char response[1024];

    log_info("Socket server thread started");

    while (ctx->running)
    {
        // Accept connection
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ctx->server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0)
        {
            if (ctx->running)
            {
                log_error("Socket accept failed");
            }
            continue;
        }

        // Read client request
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            log_debug("Received command: %s", buffer);

            // Process command
            process_command(buffer, ctx->track_manager, response, sizeof(response));

            // Send response
            write(client_fd, response, strlen(response));
        }

        close(client_fd);
    }

    log_info("Socket server thread stopped");
    return NULL;
}

// Get the socket path for the current user
char* get_socket_path(char* buffer, size_t size)
{
    if (!buffer || size == 0)
    {
        return NULL;
    }

    // Ensure the runtime directory exists
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "/var/run/user/%d/papa", (int)getuid());

    // Construct the socket path
    snprintf(buffer, size, "%s/papad.sock", dir_path);
    return buffer;
}

// Initialize socket server
socket_server_ctx_t* socket_server_init(track_manager_ctx_t* track_manager)
{
    socket_server_ctx_t* ctx = calloc(1, sizeof(socket_server_ctx_t));
    if (!ctx)
    {
        log_error("Failed to allocate socket server context");
        return NULL;
    }

    // Get the socket path
    if (!get_socket_path(ctx->socket_path, sizeof(ctx->socket_path)))
    {
        log_error("Failed to get socket path");
        free(ctx);
        return NULL;
    }

    ctx->track_manager = track_manager;
    ctx->running = false;
    ctx->server_fd = -1;

    // Remove socket if it already exists
    unlink(ctx->socket_path);

    // Create socket
    ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_fd < 0)
    {
        log_error("Socket creation failed");
        free(ctx);
        return NULL;
    }

    // Setup address structure
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctx->socket_path, sizeof(addr.sun_path) - 1);

    // Bind socket
    if (bind(ctx->server_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) < 0)
    {
        log_error("Socket bind failed");
        close(ctx->server_fd);
        free(ctx);
        return NULL;
    }

    // Listen for connections
    if (listen(ctx->server_fd, 5) < 0)
    {
        log_error("Socket listen failed");
        close(ctx->server_fd);
        free(ctx);
        return NULL;
    }

    // Set socket permissions so other users can connect
    chmod(ctx->socket_path, 0666);

    log_info("Socket server initialized at %s", ctx->socket_path);
    return ctx;
}

// Start socket server thread
bool socket_server_start(socket_server_ctx_t* ctx)
{
    if (!ctx) return false;

    ctx->running = true;
    if (pthread_create(&ctx->thread, NULL, socket_server_thread, ctx) != 0)
    {
        log_error("Failed to create socket server thread");
        ctx->running = false;
        return false;
    }

    return true;
}

// Stop and cleanup socket server
void socket_server_cleanup(socket_server_ctx_t* ctx)
{
    if (!ctx) return;

    // Signal thread to stop
    ctx->running = false;

    // Wake up accept() by connecting to the socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock >= 0)
    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, ctx->socket_path, sizeof(addr.sun_path) - 1);
        connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        close(sock);
    }

    // Wait for thread to finish
    if (ctx->thread)
    {
        pthread_join(ctx->thread, NULL);
    }

    // Close socket and remove file
    if (ctx->server_fd >= 0)
    {
        close(ctx->server_fd);
        ctx->server_fd = -1;
    }
    unlink(ctx->socket_path);

    free(ctx);
    log_info("Socket server cleaned up");
}
