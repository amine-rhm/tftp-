#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <netdb.h>  // Pour getaddrinfo

#define DATA_SIZE 512
#define MAX_FILENAME 255
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

typedef struct {
    unsigned short int opcode;
    char filename[MAX_FILENAME];
    char zero_0;
    char mode[MAX_FILENAME];
    char zero_1;
} TFTP_Request;

typedef struct {
    unsigned short int opcode;
    unsigned short int block;
    char data[DATA_SIZE];
} TFTP_Data;

typedef struct {
    unsigned short int opcode;
    unsigned short int block;
} TFTP_Ack;

static int sockfd;
static struct sockaddr_in server_addr;
static socklen_t server_len;
static int timeout = 0;
jmp_buf timeoutbuf, endbuf;

void timer(int sig) {
    switch(sig) {
        case SIGALRM: {
            timeout++;
            if (timeout >= 5) {
                printf("Retransmission timed out.\n");
                timeout = 0;
                alarm(0);
                longjmp(endbuf, sig);
            }
            printf("Retransmitting.\n");
            longjmp(timeoutbuf, sig);
        } break;
        default: break;
    }
}

int make_socket(struct sockaddr_in *s, char *host, int port) {
    struct addrinfo hints, *res;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    // Résoudre l'adresse de l'hôte
    if ((status = getaddrinfo(host, NULL, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    // Extraire l'adresse IP de la première structure de résultats
    s->sin_family = AF_INET;
    s->sin_port = htons(port);
    s->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    memset(&(s->sin_zero), 0, 8);

    freeaddrinfo(res);
    return 0;
}

void request_init(TFTP_Request *r, unsigned short int opcode, char *filename) {
    memset(r, 0, sizeof(TFTP_Request));
    r->opcode = opcode;
    strcpy(r->filename, filename);
    strcpy(r->mode, "octet");
}

void send_request(TFTP_Request *r) {
    char buf[sizeof(TFTP_Request)];
    memset(buf, 0, sizeof(buf));
    *(short*)buf = htons(r->opcode);
    memcpy(buf + 2, r->filename, strlen(r->filename) + 1);
    memcpy(buf + 2 + strlen(r->filename) + 1, r->mode, strlen(r->mode) + 1);

    int n = sendto(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&server_addr, server_len);
    if (n < 0) {
        perror("sendto");
        exit(1);
    }
}

void recv_data(char *filename) {
    TFTP_Data dataPacket;
    TFTP_Ack ack;
    int block = 1;
    FILE *file = fopen(filename, "wb");  // Utilisation du vrai nom du fichier

    if (file == NULL) {
        perror("fopen");
        exit(1);
    }

    while (1) {
        int n = recvfrom(sockfd, &dataPacket, sizeof(dataPacket), 0, (struct sockaddr *)&server_addr, &server_len);
        if (n < 0) {
            perror("recvfrom");
            break;
        }

        if (ntohs(dataPacket.opcode) == DATA) {
            fwrite(dataPacket.data, 1, n - 4, file); // Exclure l'opcode et le bloc
            ack.opcode = htons(ACK);
            ack.block = dataPacket.block;
            sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, server_len);

            if (n < DATA_SIZE + 4) break; // Dernier bloc
        }
    }

    fclose(file);
}

void send_data(char *filename) {
    TFTP_Data dataPacket;
    TFTP_Ack ack;
    int block = 1;
    FILE *file = fopen(filename, "rb");

    if (file == NULL) {
        perror("fopen");
        exit(1);
    }

    int retries = 5;
    while (retries > 0) {
        int n = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &server_len);
        if (n < 0) {
            perror("recvfrom");
            retries--;
            printf("Pas d'ACK(0) reçu, réessai %d/5\n", 5 - retries);
            continue;
        }

        printf("ACK reçu : opcode=%d, bloc=%d\n", ntohs(ack.opcode), ntohs(ack.block));

        if (ntohs(ack.opcode) == ACK && ntohs(ack.block) == 0) {
            printf("ACK(0) reçu, début de l'envoi des données\n");
            break;  // On peut envoyer les données maintenant
        } else {
            printf(" ACK erroné ou pour un mauvais bloc, réessai\n");
        }
    }

  
    while (1) {
        int bytesRead = fread(dataPacket.data, 1, DATA_SIZE, file);
        dataPacket.opcode = htons(DATA);
        dataPacket.block = htons(block);

        retries = 5;
        while (retries > 0) {
         
            printf(" Envoi du paquet DATA : opcode=%d, bloc=%d, taille=%d\n", 
                   ntohs(dataPacket.opcode), ntohs(dataPacket.block), bytesRead);
            int n = sendto(sockfd, &dataPacket, bytesRead + 4, 0, 
                           (struct sockaddr *)&server_addr, server_len);
            if (n < 0) {
                perror("sendto");
                retries--;
                continue;
            }

            n = recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &server_len);
            if (n < 0) {
                perror("recvfrom");
                retries--;
                printf("Pas d'ACK reçu pour le bloc %d, réessai %d/5\n", block, 5 - retries);
                continue;
            }

            uint16_t ack_opcode = ntohs(ack.opcode);
            uint16_t ack_block_num = ntohs(ack.block);

            printf("ACK reçu : opcode=%d, bloc=%d (attendu: %d)\n", ack_opcode, ack_block_num, block);

            if (ack_opcode == ACK && ack_block_num == block) {
                printf("Correct ACK pour le bloc %d\n", block);
                break;  // L'ACK a été reçu, on passe au bloc suivant
            } else {
                printf("ACK erroné pour le bloc %d, réessai\n", block);
                retries--;
            }
        }

        block++;  // Incrémenter le numéro de bloc

        if (bytesRead < DATA_SIZE) {
            printf("Dernier paquet envoyé, fin du transfert\n");
            break;  // Si moins de 512 octets lus, c'est le dernier bloc
        }
    }

    fclose(file);
}
void run_client(char *host, int port, char *filename, int is_rrq) {
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    make_socket(&server_addr, host, port);
    server_len = sizeof(server_addr);

    TFTP_Request request;
    if (is_rrq) {
        request_init(&request, RRQ, filename); // Demande de lecture
    } else {
        request_init(&request, WRQ, filename); // Demande d'écriture
    }

    send_request(&request);

    if (is_rrq) {
        recv_data(filename); // Recevoir les données avec le vrai nom de fichier
    } else {
        send_data(filename); // Envoyer les données avec le vrai nom de fichier
    }

    close(sockfd);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Usage: %s <server_ip> <port> <filename> <RRQ/WRQ>\n", argv[0]);
        exit(1);
    }

    char *host = argv[1];
    int port = atoi(argv[2]);
    char *filename = argv[3];
    int is_rrq = (strcmp(argv[4], "RRQ") == 0) ? 1 : 0;

    run_client(host, port, filename, is_rrq);

    return 0;
}
