#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>

#define TFTP_PORT 6968
#define DATA_SIZE 512
#define TIMEOUT 5

// Les différents types de paquets TFTP
#define RRQ 1    // Demande de lecture
#define WRQ 2    // Demande d'écriture
#define DATA 3   // Paquet de données
#define ACK 4    // Accusé de réception
#define ERROR 5  // Erreur


// TFTP packet structures
typedef struct {
    uint16_t opcode;
    char filename[256];
    char mode[12];
} TFTP_Request;

typedef struct {
    uint16_t opcode;
    uint16_t block_num;
    char data[DATA_SIZE];
} TFTP_Data;

typedef struct {
    uint16_t opcode;
    uint16_t block_num;
} TFTP_Ack;

typedef struct {
    uint16_t opcode;
    uint16_t error_code;
    char error_msg[100];
} TFTP_Error;

// Global variables
jmp_buf timeout_jump;

void handle_timeout(int sig) {
    siglongjmp(timeout_jump, 1);
}

void send_error(int sock, struct sockaddr_in *client_addr, socklen_t client_len, uint16_t error_code, const char *error_msg) {
    TFTP_Error error_packet;
    error_packet.opcode = htons(ERROR);
    error_packet.error_code = htons(error_code);
    strncpy(error_packet.error_msg, error_msg, sizeof(error_packet.error_msg) - 1);
    error_packet.error_msg[sizeof(error_packet.error_msg) - 1] = '\0';

    sendto(sock, &error_packet, 4 + strlen(error_packet.error_msg) + 1, 0, (struct sockaddr *)client_addr, client_len);
}

void handle_rrq(int sock, TFTP_Request *request, struct sockaddr_in *client_addr, socklen_t client_len) {
    FILE *file = fopen(request->filename, "rb");
    if (!file) {
        perror("Erreur fopen");
        send_error(sock, client_addr, client_len, 1, "File not found");
        return;
    }

    TFTP_Data data_packet;
    data_packet.opcode = htons(DATA);
    uint16_t block_num = 1;
    size_t bytes_read;

    while ((bytes_read = fread(data_packet.data, 1, DATA_SIZE, file)) > 0) {
        printf("Sending block %d, bytes read: %zu\n", block_num, bytes_read);

        data_packet.block_num = htons(block_num);

        while (1) {
            if (sigsetjmp(timeout_jump, 1) != 0) {
                printf("Timeout, resending block %d\n", block_num);
            }

            alarm(TIMEOUT);
            ssize_t sent_bytes = sendto(sock, &data_packet, 4 + bytes_read, 0, (struct sockaddr *)client_addr, client_len);
            alarm(0);

            if (sent_bytes < 0) {
                perror("sendto failed");
                continue;
            }

            // Recevoir l'ACK
            TFTP_Ack ack_packet;
            ssize_t recv_bytes = recvfrom(sock, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)client_addr, &client_len);
            if (recv_bytes < 0) {
                if (errno == EINTR) continue; // Timeout, réessayer
                perror("recvfrom failed");
                break;
            }

            uint16_t ack_opcode = ntohs(ack_packet.opcode);
            uint16_t ack_block_num = ntohs(ack_packet.block_num);

            printf("Received raw ACK packet: opcode=%d, block_num=%d (raw: %04x %04x)\n",
                   ack_opcode, ack_block_num, ack_packet.opcode, ack_packet.block_num);

            if (ack_opcode == ACK && ack_block_num == block_num) {
                printf("Received valid ACK for block %d\n", block_num);
                break; // ACK valide, passer au bloc suivant
            } else {
                printf("Invalid ACK received: opcode=%d, block=%d (expected block=%d)\n", 
                        ack_opcode, ack_block_num, block_num);
            }
        }

        block_num++;
    }

    fclose(file);
}
void handle_wrq(int sock, TFTP_Request *request, struct sockaddr_in *client_addr, socklen_t client_len) {
    FILE *file = fopen(request->filename, "wb");
    if (!file) {
        send_error(sock, client_addr, client_len, 2, "Cannot create file");
        return;
    }

    printf(" Received WRQ for file: %s, mode: %s\n", request->filename, request->mode);

    TFTP_Ack ack_packet;
    ack_packet.opcode = htons(ACK);
    ack_packet.block_num = htons(0);

    if (sendto(sock, &ack_packet, 4, 0, (struct sockaddr *)client_addr, client_len) < 0) {
        perror("Failed to send initial ACK");
        fclose(file);
        return;
    }
    printf("Sent initial ACK for WRQ (block 0)\n");

    TFTP_Data data_packet;
    uint16_t expected_block = 1;
    ssize_t recv_bytes;

    while (1) {
        recv_bytes = recvfrom(sock, &data_packet, sizeof(data_packet), 0, 
                              (struct sockaddr *)client_addr, &client_len);
        if (recv_bytes < 0) {
            perror("recvfrom failed");
            break;
        }

        uint16_t data_opcode = ntohs(data_packet.opcode);
        uint16_t data_block_num = ntohs(data_packet.block_num);
        size_t data_len = recv_bytes - 4;  // Exclure l'opcode et le numéro de bloc

        printf("Received DATA packet: opcode=%d, block=%d, data length=%zu\n", 
               data_opcode, data_block_num, data_len);

        if (data_opcode != DATA) {
            printf("Unexpected opcode %d (expected DATA=%d)\n", data_opcode, DATA);
            send_error(sock, client_addr, client_len, 4, "Unexpected packet type");
            break;
        }

        if (data_block_num != expected_block) {
            printf("Block number mismatch: received=%d, expected=%d\n", data_block_num, expected_block);
            send_error(sock, client_addr, client_len, 4, "Wrong block number");
            continue;  // Attendre le bon bloc
        }

        if (data_len > 0) {
            size_t written = fwrite(data_packet.data, 1, data_len, file);
            if (written != data_len) {
                printf("fwrite() error: expected=%zu, written=%zu\n", data_len, written);
                send_error(sock, client_addr, client_len, 2, "File write error");
                break;
            }
        } else {
            printf("Empty DATA packet received\n");
        }

        ack_packet.block_num = htons(expected_block);
        if (sendto(sock, &ack_packet, 4, 0, (struct sockaddr *)client_addr, client_len) < 0) {
            perror("Failed to send ACK");
            break;
        }
        printf("Sent ACK for block %d\n", expected_block);

        expected_block++;

        if (data_len < DATA_SIZE) {
            printf("Last DATA packet received, transfer complete.\n");
            break;
        }
    }

    fclose(file);
    printf("WRQ terminé pour le fichier %s\n", request->filename);
}
void process_request(int sock, struct sockaddr_in *client_addr, socklen_t client_len, char *buffer, size_t len) {
    TFTP_Request request;
    request.opcode = ntohs(*(uint16_t *)buffer);
    
    // Extraction du filename et du mode à partir du buffer
    size_t filename_len = strlen(buffer + 2);
    strncpy(request.filename, buffer + 2, filename_len);
    request.filename[filename_len] = '\0';  // Null-terminate the filename
    
    size_t mode_len = strlen(buffer + 2 + filename_len + 1);
    strncpy(request.mode, buffer + 2 + filename_len + 1, mode_len);
    request.mode[mode_len] = '\0';  // Null-terminate the mode

    printf("Received request: opcode=%d, filename=%s, mode=%s\n",request.opcode, request.filename, request.mode);

    if (request.opcode == RRQ) {
        handle_rrq(sock, &request, client_addr, client_len);
    } else if (request.opcode == WRQ) {
        handle_wrq(sock, &request, client_addr, client_len);
    } else {
        send_error(sock, client_addr, client_len, 4, "Invalid request");
    }
}

int main() {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    char buffer[516];
    socklen_t client_len = sizeof(client_addr);

    signal(SIGALRM, handle_timeout);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TFTP_PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("TFTP server listening on port %d\n", TFTP_PORT);

   while (1) {
    ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_len);
    if (len < 0) {
        perror("recvfrom failed");
        continue;
    }

    uint16_t opcode = ntohs(*(uint16_t *)buffer);

    if (opcode == RRQ || opcode == WRQ) {
        // Traiter les requêtes RRQ et WRQ
        process_request(sock, &client_addr, client_len, buffer, len);
         
    } /*else if (opcode == ACK) {
        // Ignorer les ACK dans le serveur principal (ils sont gérés dans handle_rrq et handle_wrq)
        printf("Received ACK packet\n");
    } else if (opcode == DATA) {
        // Ignorer les DATA dans le serveur principal (ils sont gérés dans handle_wrq)
        printf("Received DATA packet\n");
    } else {
        printf("Unknown opcode: %d\n", opcode);
    }*/
}
}
