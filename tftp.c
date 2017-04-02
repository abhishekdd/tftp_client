#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define OP_RRQ 		01
#define OP_WRQ 		02
#define OP_DATA 	03
#define OP_ACK 		04
#define OP_ERROR 	05

#define HEADER_SIZE 4
#define BLOCK_SIZE 	512
#define DATA_SIZE 	BLOCK_SIZE + HEADER_SIZE
#define ACK_SIZE 	HEADER_SIZE

#define MAX_RETRY 	3
#define DEBUG_MODE  0

int  get_server_addr(char *host, char *port, struct sockaddr *server_addr);
void get_file(uint16_t sockfd, struct sockaddr serv_addr, uint16_t diskfd, 
			  char *filename);
void put_file(uint16_t sockfd, struct sockaddr serv_addr, uint16_t diskfd, 
			  char *filename);

void send_XRQ(uint16_t sockfd, struct sockaddr *serv_addr, char *filename, 
			  uint16_t op);
void send_ACK(uint16_t sockfd, struct sockaddr *serv_addr, uint16_t blockNo);
void send_DATA(uint8_t *databuf, uint16_t tosend, uint16_t blockNo, 
			   uint16_t sockfd, struct sockaddr *serv_addr);
int  recv_ACK(uint8_t *buffer, uint16_t sockfd, struct sockaddr *serv_addr);
int  recv_DATA(uint8_t *buffer, uint16_t sockfd, struct sockaddr *serv_addr);

void print_packet(uint8_t *buffer, uint16_t size);

const char *mode = "octet";

int main(int argc, char *argv[])
{
	struct sockaddr servaddr;
	uint16_t sockfd, diskfd;
	struct timeval timeout;

	if (argc != 5) {
		fprintf(stderr, "Usage: %s <host> <port> get/put <filename>\n", 
				argv[1]);
		return EXIT_FAILURE;
	}

	sockfd = get_server_addr(argv[1], argv[2], &servaddr);
	
	timeout.tv_sec  = 0;
	timeout.tv_usec = 100000; 		// 100 mili-seconds
	if ( setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, 
				   sizeof(struct timeval)) == -1 ) {
		perror("setsockopt('RCVTIMEO')");
		return EXIT_FAILURE;
	}

	if ( strncmp(argv[3], "get", 3) == 0 ) {
		diskfd = open(argv[4], O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);
		if (diskfd == -1) {
			perror("Error-open(WR)");
			return EXIT_FAILURE;
		}
		
		get_file(sockfd, servaddr, diskfd, argv[4]);
	} else if ( strncmp(argv[3], "put", 3) == 0 ) {
		diskfd = open(argv[4], O_RDONLY);
		if (diskfd == -1) {
			perror("Error-open(RD)");
			return EXIT_FAILURE;
		}

		put_file(sockfd, servaddr, diskfd, argv[4]);
	} else {
		fprintf(stderr, "Unknown operation. Use either 'get' or 'put'\n");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


int get_server_addr(char *host, char *port, struct sockaddr *server_addr)
{
	uint16_t sockfd, retval;
	struct addrinfo hints, *servinfo, *ptr;

	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET; 		// IPv4 addresses
	hints.ai_socktype = SOCK_DGRAM; 	// UDP based datagram socket

	if ( (retval = getaddrinfo(host, port, &hints, &servinfo)) != 0 ) {
		fprintf(stderr, "Error-getaddrinfo(): %s\n", gai_strerror(retval));
		exit(EXIT_FAILURE);
	}

    for (ptr = servinfo; ptr != NULL; ptr = ptr->ai_next) {
        if ( (sockfd = socket(ptr->ai_family, ptr->ai_socktype,
                             ptr->ai_protocol)) == -1 ) {
            perror("Error-socket()");
            continue;
        }
        break;
    }

    if (ptr == NULL) {
        fprintf(stderr, "Error: failed to create socket\n");
        exit(EXIT_FAILURE);
    }

	*server_addr = *(ptr->ai_addr);
	freeaddrinfo(servinfo);

	return sockfd;
}


void get_file(uint16_t sockfd, struct sockaddr serv_addr, uint16_t diskfd, 
			  char *filename)
{
	uint8_t data_buffer[DATA_SIZE];
	uint16_t lastBlockNo, currentBlockNo, retryCount, op, n;

	///////////////////////////////////////////////////////////////////////////
	if (DEBUG_MODE) {
		printf("sockfd: %d\n", sockfd);
		printf("diskfd: %d\n", diskfd);
		struct sockaddr_in *sain = (struct sockaddr_in *) &serv_addr;
		printf("IP  : %s\n", inet_ntoa(sain->sin_addr));
		printf("Port: %d\n", ntohs(sain->sin_port));
		printf("Filename: '%s'\n", filename);
	}
	///////////////////////////////////////////////////////////////////////////
	printf("Downloading file '%s' ... \n", filename);

	lastBlockNo = 0;
	retryCount = 0;

	send_XRQ(sockfd, &serv_addr, filename, OP_RRQ);
	if (DEBUG_MODE) {
		printf("sent RRQ[%d]\n", retryCount);
	}
	while (1) {
		n = recv_DATA(data_buffer, sockfd, &serv_addr);
		if (n == -1 && retryCount < MAX_RETRY) {
			if (lastBlockNo == 0) {
				send_XRQ(sockfd, &serv_addr, filename, OP_RRQ);
				if (DEBUG_MODE) {
					printf("sent RRQ[%d]r\n", retryCount);
				}
			} else {
				send_ACK(sockfd, &serv_addr, lastBlockNo);
				if (DEBUG_MODE) {
					printf("sent ACK{%d}[%d]r\n", lastBlockNo, retryCount);
				}
			}

			retryCount++;
			continue;
		}

		if (retryCount == MAX_RETRY) {
			fprintf(stderr, "Maximum retransmission limit of %d reached.\n", 
					MAX_RETRY);
			exit(EXIT_FAILURE);
		}
		retryCount = 0;

		op = data_buffer[0];
		op <<= 8;
		op |= data_buffer[1];

		if (op == OP_ERROR) {
			op = data_buffer[2];
			op <<= 8;
			op |= data_buffer[3];

			fprintf(stderr, "Error-message: [%d] %s\n", op, 
					data_buffer + HEADER_SIZE);
			exit(0);
		} else if (op != OP_DATA) {
			fprintf(stderr, "Expected DATA(%d) received [%d]\n", OP_DATA, op);
			exit(0);
		}

		currentBlockNo = data_buffer[2];
		currentBlockNo <<= 8;
		currentBlockNo |= data_buffer[3];
		if (DEBUG_MODE) {
			printf("recv DATA{%d}<%d bytes>\n", currentBlockNo, n-HEADER_SIZE);
		}

		if (currentBlockNo == lastBlockNo) {
			send_ACK(sockfd, &serv_addr, lastBlockNo);
			if (DEBUG_MODE) {
				printf("sent ACK{%d}[%d]p\n", lastBlockNo, retryCount);
			}
		} else {
			if (write(diskfd, data_buffer + HEADER_SIZE, n-HEADER_SIZE) == -1) {
				perror("write()");
				exit(EXIT_FAILURE);
			}

			send_ACK(sockfd, &serv_addr, currentBlockNo);
			if (DEBUG_MODE) {
				printf("sent ACK{%d}[%d]\n", currentBlockNo, retryCount);
			}

			lastBlockNo = currentBlockNo;

			if (n - HEADER_SIZE < BLOCK_SIZE) {
				break;
			}
		}
	}

	printf("Download complete.\n");
}


void put_file(uint16_t sockfd, struct sockaddr serv_addr, uint16_t diskfd, 
			  char *filename)
{
	uint8_t ack_buffer[DATA_SIZE], data_buffer[BLOCK_SIZE];
	uint16_t dataBlockNo, ackBlockNo, retryCount, op, bytes_read, n;

	///////////////////////////////////////////////////////////////////////////
	if (DEBUG_MODE) {
		printf("sockfd: %d\n", sockfd);
		printf("diskfd: %d\n", diskfd);
		struct sockaddr_in *sain = (struct sockaddr_in *) &serv_addr;
		printf("IP  : %s\n", inet_ntoa(sain->sin_addr));
		printf("Port: %d\n", ntohs(sain->sin_port));
		printf("Filename: '%s'\n", filename);
	}
	///////////////////////////////////////////////////////////////////////////
	printf("Uploading file '%s' ... \n", filename);

	dataBlockNo = retryCount = 0;
	bytes_read = BLOCK_SIZE;

	send_XRQ(sockfd, &serv_addr, filename, OP_WRQ);
	if (DEBUG_MODE) {
		printf("sent WRQ[%d]\n", 0);
	}

	while (1) {
		n = recv_ACK(ack_buffer, sockfd, &serv_addr);
		if (n == -1 && retryCount < MAX_RETRY) {
			if (dataBlockNo == 0) {
				send_XRQ(sockfd, &serv_addr, filename, OP_WRQ);
				if (DEBUG_MODE) {
					printf("sent WRQ[%d]r\n", retryCount);
				}
			} else {
				send_DATA(data_buffer, bytes_read, dataBlockNo, sockfd, &serv_addr);
				if (DEBUG_MODE) {
					printf("sent DATA{%d}<%d bytes>r\n", dataBlockNo, bytes_read);
				}
			}

			retryCount++;
			continue;
		}

		if (retryCount == MAX_RETRY) {
			fprintf(stderr, "Maximum retransmission limit of %d reached.\n", 
					MAX_RETRY);
			exit(EXIT_FAILURE);
		}
		retryCount = 0;

		op = ack_buffer[0];
		op <<= 8;
		op |= ack_buffer[1];

		if (op == OP_ERROR) {
			op = ack_buffer[2];
			op <<= 8;
			op |= ack_buffer[3];

			fprintf(stderr, "Error-message: [%d] %s\n", op, ack_buffer + HEADER_SIZE);
			exit(0);
		} else if (op != OP_ACK) {
			fprintf(stderr, "Expected ACK(%d) received [%d]\n", OP_ACK, op);
			exit(0);
		}

		ackBlockNo = ack_buffer[2];
		ackBlockNo <<= 8;
		ackBlockNo |= ack_buffer[3];
		if (DEBUG_MODE) {
			printf("recv ACK{%d}[%d]\n", ackBlockNo, retryCount);
		}

		if (ackBlockNo == dataBlockNo) {
			if (bytes_read < BLOCK_SIZE) {
				break;
			}

			if ( (bytes_read = read(diskfd, data_buffer, BLOCK_SIZE)) == -1 ) {
				perror("read()");
				exit(EXIT_FAILURE);
			}

			dataBlockNo++;
			send_DATA(data_buffer, bytes_read, dataBlockNo, sockfd, &serv_addr);

			if (DEBUG_MODE) {
				printf("sent DATA{%d}<%d bytes>\n", dataBlockNo, bytes_read);
			}

		} else {
			send_DATA(data_buffer, bytes_read, dataBlockNo, sockfd, &serv_addr);
			if (DEBUG_MODE) {
				printf("sent DATA{%d}<%d bytes>p\n", dataBlockNo, bytes_read);
			}
		}
	}

	printf("Upload complete.\n");
}


void send_XRQ(uint16_t sockfd, struct sockaddr *serv_addr, 
			  char *filename, uint16_t op)
{
	uint8_t buffer[DATA_SIZE];
	socklen_t addrlen;

	memset(buffer, 0, DATA_SIZE);
	sprintf((char *) buffer, "%c%c%s%c%s", op >> 8, op & 255, filename, '\0', mode);
	// print_packet(buffer, DATA_SIZE);

	addrlen = sizeof (*serv_addr);
	if ( sendto(sockfd, buffer, DATA_SIZE, 0, serv_addr, 
			    addrlen) == -1 ) {
		perror("send_RRQ");
		exit(EXIT_FAILURE);
	}
}


int recv_DATA(uint8_t *buffer, uint16_t sockfd, struct sockaddr *serv_addr)
{
	uint16_t bytes_read;
	socklen_t addrlen;

	addrlen = sizeof (*serv_addr);
	if ( (bytes_read = recvfrom(sockfd, buffer, DATA_SIZE, 0, serv_addr, 
								&addrlen)) == -1 ) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return -1;
		}

		perror("recv_DATA");
		exit(EXIT_FAILURE);
	}

	return bytes_read;
}


int recv_ACK(uint8_t *buffer, uint16_t sockfd, struct sockaddr *serv_addr)
{
	uint16_t bytes_read;
	socklen_t addrlen;

	addrlen = sizeof (*serv_addr);
	if ( (bytes_read = recvfrom(sockfd, buffer, DATA_SIZE, 0, serv_addr, 
								&addrlen)) == -1 ) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return -1;
		}

		perror("recv_ACK");
		exit(EXIT_FAILURE);
	}

	return bytes_read;
}


void send_DATA(uint8_t *databuf, uint16_t tosend, uint16_t blockNo, 
			  uint16_t sockfd, struct sockaddr *serv_addr)
{
	uint8_t buffer[DATA_SIZE];
	socklen_t addrlen;

	memset(buffer, 0, DATA_SIZE);
	buffer[0] = OP_DATA >> 8;
	buffer[1] = OP_DATA & 255;
	buffer[2] = blockNo >> 8;
	buffer[3] = blockNo & 255;
	memcpy(buffer + HEADER_SIZE, databuf, tosend);

	addrlen = sizeof (*serv_addr);
	if ( sendto(sockfd, buffer, HEADER_SIZE + tosend, 0, serv_addr, 
			    addrlen) == -1 ) {
		perror("send_DATA");
		exit(EXIT_FAILURE);
	}
}


void send_ACK(uint16_t sockfd, struct sockaddr *serv_addr, uint16_t blockNo) 
{
	uint8_t buffer[ACK_SIZE + 1];
	socklen_t addrlen;

	memset(buffer, 0, ACK_SIZE+1);
	sprintf((char *) buffer, "%c%c%c%c", OP_ACK >> 8, OP_ACK & 255, blockNo >> 8, 
			blockNo & 255);

	addrlen = sizeof (*serv_addr);
	if ( sendto(sockfd, buffer, ACK_SIZE, 0, serv_addr, addrlen) == -1 ) {
		perror("send_ACK");
		exit(EXIT_FAILURE);
	}
}


void print_packet(uint8_t *buff, uint16_t size)
{
	int i;
	for (i = 0; i < size; i++) {
		if (i % 20 == 0)
			putchar('\n');

		printf(" <%02X>", buff[i]);
	}
	putchar('\n');
}