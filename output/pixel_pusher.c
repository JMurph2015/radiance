#include "pixel_pusher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <SDL2/SDL.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "output/config.h"
#include "util/err.h"

// This file implements a PixelPusher output.  It is designed to discover only 1 PixelPusher,
// but to use multiple grids attached to that PixelPusher

static struct pp_discovery_packet pp_info;

static struct pp_device * grid_devices = NULL;
static size_t n_grid_devices = 0;

// Reusable state for sending data packets
static int out_fd = -1;
static uint32_t seq_num = 0;
static struct sockaddr_in out_addr;

// This is dynamic because we need to allocate memory based on
// the number of pixels per strip.
static uint8_t * out_packet;

// See output_pp_do_frame for why we need this
static struct timespec packet_interval = { .tv_sec = 0, .tv_nsec = 500*1000};

static void numToHex(uint8_t* outstr, int val){
    const char str[] = "0123456789ABCDEF";
    outstr[0] = str[val / 16];
    outstr[1] = str[val % 16];
    outstr[2] = '\0';
}

static int pp_find_pusher() {
    INFO("Initializing PixelPusher");

    // Try to open a socket
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        PERROR("Error opening PixelPusher discovery socket");
        return -1;
    }

    // Set its timeout
    struct timeval tv;
    tv.tv_sec = output_config.pixel_pusher.discovery_seconds;
    if (!tv.tv_sec) {
        ERROR("PixelPusher discovery seconds must be non-zero");
        close(server_fd);
        return -1;
    }

    tv.tv_usec = 0;
    if (setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(struct timeval)) < 0) {
        PERROR("Error setting PixelPusher discovery socket timeout");
        close(server_fd);
        return -1;
    }

    // Socket address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(output_config.pixel_pusher.port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof server_addr) < 0) {
        PERROR("Error binding PixelPusher discovery socket");
        close(server_fd);
        return -1;
    }

    // Read
    size_t expected_bytes = sizeof pp_info;
    int bytes_read = read(server_fd, &pp_info, expected_bytes);
    if (bytes_read < (int) expected_bytes) {
        PERROR("Expected to read %zu bytes from PixelPusher but read %d bytes", expected_bytes, bytes_read);
        close(server_fd);
        return -1;
    }

    close(server_fd);

    struct in_addr ip_addr;
    ip_addr.s_addr = pp_info.header.ip_addr;
    INFO("Found PixelPusher at IP address %s", inet_ntoa(ip_addr));

    return 0;
}

static int pp_add_grids() {
    n_grid_devices = output_config.n_pixel_pusher_grids;
    grid_devices = calloc(n_grid_devices, sizeof *grid_devices);
    if (grid_devices == NULL) MEMFAIL();

    for (size_t i = 0; i < n_grid_devices; i++) {
        struct pp_device* device = &grid_devices[i];
        memset(device, 0, sizeof *device);

        // Hook ourselves into the output_device_head list
        if (!output_config.pixel_pusher_grids[i].configured)
            continue;
        if (output_device_head != NULL)
            output_device_head->prev = &device->base;
        device->base.next = output_device_head;
        output_device_head = &device->base;
        device->base.prev = NULL;

        // General device configuration
        device->base.active = true;
        device->base.ui_name = output_config.pixel_pusher_grids[i].ui_name;
        device->strip_num = output_config.pixel_pusher_grids[i].strip_num;

        // Geometry and pixel arrangement
        // TODO: Maybe do some sanity checking on configuration
        // TODO: Maybe read this from the device itself (via the PixelPusher struct in packet.py),
        // although because we obviously have to set the position manually in the config file
        // it's maybe cleaner to keep width and height there as well
        device->width = output_config.pixel_pusher_grids[i].width;
        device->height = output_config.pixel_pusher_grids[i].height;
        device->base.pixels.length = device->width * device->height;
        device->base.vertex_head = output_config.pixel_pusher_grids[i].vertexlist;

        int res = output_device_arrange_grid(&device->base, device->width, device->height);
        if (res < 0) {
            ERROR("Unable to arrange pixels for PixelPusher grid %zu", i);
            device->base.active = false;
            return -1;
        }
    }

    return 0;
}

static int pp_init_out() {
    seq_num = 0;

    // Try to open a socket
    if (out_fd > 0) {
        close(out_fd);
    }
    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) {
        PERROR("Error opening PixelPusher data socket");
        return -1;
    }

    // Target address
    // This is only called if pp_find_pusher finishes successfully, so
    // pp_info is set
    out_addr.sin_family = AF_INET;
    out_addr.sin_addr.s_addr = pp_info.header.ip_addr;
    out_addr.sin_port = htons(pp_info.info.my_port);

    // 4 bytes for the sequence number; 2 strips each with a 1 byte strip
    // number and 3 bytes (RGB) for each pixel
    out_packet = calloc(1, 4 + 2*(1+3*grid_devices[0].base.pixels.length));
    if (out_packet == NULL) MEMFAIL();

    return 0;
}

int output_pp_init() {
    if (pp_find_pusher() < 0) {
        return -1;
    }

    if (pp_add_grids() < 0) {
        return -1;
    }

    return pp_init_out();
}

void output_pp_term() {
    INFO("Terminating PixelPusher");

    for (size_t i = 0; i < n_grid_devices; i++) {
        struct output_device base = grid_devices[i].base;

        free(base.pixels.xs);
        free(base.pixels.ys);
        free(base.pixels.colors);

        if (base.prev != NULL)
            base.prev->next = base.next;
        if (base.next != NULL)
            base.next->prev = base.prev;
    }
    free(grid_devices);
    grid_devices = NULL;
    n_grid_devices = 0;


    if (out_fd) {
        close(out_fd);
        out_fd = -1;
    }

    if (out_packet) {
        free(out_packet);
        out_packet = NULL;
    }
}

int output_pp_do_frame() {
    seq_num++;

    ((char *) out_packet)[0] = seq_num;
    // We'll use this to keep track of our location in the packet buffer
    // as we fill it.  The sequence number is 4 bytes so we start after that.
    size_t out_packet_idx = 4;

    for (size_t i = 0; i < n_grid_devices; i++) {
        struct pp_device * device = &grid_devices[i];

        // Make sure it was successfully initialized
        if (!device->base.active) continue;

        // Copy the data to send into a buffer - sadly SDL_Color is rgba
        // so we can't just send a header and it using sendmsg
        out_packet[out_packet_idx++] = device->strip_num;

        // Snake: The PixelPusher has linear strips arranged into a grid
        // by going "back and forth".
        for (int j = 0; j < device->height; j++) {
            for (int k = 0; k < device->width; k++) {
                int idx = (j % 2 ? device->width - 1 - k: k)*(device->height) + j;

                SDL_Color color = device->base.pixels.colors[idx];
                double alpha = color.a / 255.0;
                numToHex(&out_packet[out_packet_idx++],color.r * alpha);
                numToHex(&out_packet[out_packet_idx++],color.g * alpha);
                numToHex(&out_packet[out_packet_idx++],color.b * alpha);
                //out_packet[out_packet_idx++] = numToHex(color.g * alpha);
                //out_packet[out_packet_idx++] = numToHex(color.b * alpha);
            }
        }

        // Send it: the PixelPusher can take 2 grids per packet
        if ((i % 2) || (i == n_grid_devices - 1)) {
            size_t sent_bytes = sendto(out_fd, out_packet, out_packet_idx, 0, (struct sockaddr *) &out_addr, sizeof out_addr);
            if (sent_bytes < out_packet_idx) {
                PERROR("Error sending PixelPusher data");
                return -1;
            }

            // The PixelPusher doesn't have a very large Ethernet buffer, and UDP doesn't resend things,
            // so if we don't give it some time to process it'll just drop any more incoming packets. The
            // symptom of this is only some of the grids will respond and the others will stay dark or
            // flicker; if this happens increase packet_interval.  Don't bother doing this on the last
            // packet, because any reasonable framerate will have delays much longer than this (even, say
            // 1000 FPS = 1 millisecond > 500 microseconds.
            if (i != n_grid_devices - 1) {
                nanosleep(&packet_interval, NULL);
            }

            // Reset the packet buffer index to after the sequence number - it will
            // stay the same for all of the packets in this frame.
            out_packet_idx = 4;
        }
    }

    return 0;
}
