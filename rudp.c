#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>

#include "event.h"
#include "rudp.h"
#include "rudp_api.h"

#define INIT		0			// RUDP socket state: INIT.
#define DATA		1			// RUDP socket state: DATA.
#define CLOSING		2			// RUDP socket state: CLOSING.
#define WAIT_FIN_ACK	3			// RUDP socket state: WAIT_FIN_ACK.
#define FIN		4			// RUDP socket state: FIN.

#define MAX_SEQ 2147483646			// The largest possible 32-bit integer value.

typedef struct{
	struct rudp_hdr	header;			// RUDP header.
	char data[RUDP_MAXPKTSIZE];		// RUDP data; RUDP_MAXPACKETSIZE is 1000.
}__attribute__((packed)) rudp_packet;		// Since it's a 'typedef' for a struct, only rudp_packet is called.

struct send_data_list_buffer{
        rudp_packet* packet;                   	// RUDP packet structure.
	struct rudp_socket* skt;		// Pointer to the RUDP socket for this packet buffer.           
        int datalen;                            // RUDP packet data length.
	int fd;					// Filde Descriptor.	
        struct send_data_list_buffer *next;	// Pointer to structure which describes the next packet.
	int retransCount;                       // Retransmission counter.
	struct sockaddr_in* dest;	    	// The destination address for this packet.
};

struct rudp_socket{
	int fd;					// Socket file descriptor.
	struct sockaddr_in* dest;		// The destination address.
	int state;				// The socket's state.
	int window_size;			// The socket's current window size.
	int hack;				// |- Receiver: the next expected sequence number. 
						// |- Sender: the sequence number of the packet that the receiver expects.
	int synseqno;				// The RUDP SYN seuence number; used in the case when close_socket is called
						// before the SYN ACK arrives.
	int seqno;				// The sequence number of the next data packet to be added to the buffer.
	int reachedEnd;				// Boolean int variable which specifies if all packets until RUDP FIN has
						// been transmitted.
	struct send_data_list_buffer* head;	// Pointer that keeps track of the head of the packet buffer.
	int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
	int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
};

struct rudp_hdr createRUDPHeader(u_int16_t type, u_int32_t seqno);

rudp_packet* sendSYN(int fd, struct sockaddr_in* to, int seqno);

rudp_packet* createRUDPPacket(u_int16_t type, u_int32_t seqno, char* data, int datalen);

int send_ack(struct rudp_socket *rsocket, struct sockaddr_in *dest, int seqnum);

int rudp_receive_data(int fd, void *arg);

int rudp_retransmit(int argc, void *arg);

struct send_data_list_buffer* createNodeBuffer(rudp_packet* packet, struct rudp_socket* skt,
		int datalen, struct sockaddr_in* dest){
	struct send_data_list_buffer* node;
	node = (struct send_data_list_buffer *)malloc(sizeof(struct send_data_list_buffer));
	memset(node, 0x0, sizeof(struct send_data_list_buffer));
	node->packet = packet;			// Set the RUDP packet of this node.
	node->skt = skt;			// Register the RUDP socket pointer to this node.
	node->datalen = datalen;		// Register the RUDP packet data length.
	node->dest = dest;			// Register the destination address pointer.
	node->next = NULL;			// This will be the current latest packet; next==NULL.
	return node; 
}

struct send_data_list_buffer* addNode(struct send_data_list_buffer* head, 
		struct send_data_list_buffer* node){
	struct send_data_list_buffer* tmp;
	if(head==NULL){				// If the list is empty, add node to the head.
   		head = node;
	}else{					// Else, loop to the end of the list and add the node.
    		for(tmp=head; tmp->next!=NULL; tmp=tmp->next){}          
    		tmp->next= node;
	}
	return head;				// Return a pointer to the head to register in the socket.
}

struct send_data_list_buffer* removeNode(struct send_data_list_buffer* head){
	struct send_data_list_buffer* tmp;
	if(head==NULL){				// If the list is empty, just return.
 		return NULL;
	}else{					
		tmp = head;
		head = head->next;
		event_timeout_delete(&rudp_retransmit, (void*)tmp);
		free(tmp);
		return head;
	}
}

struct send_data_list_buffer* findNode(struct send_data_list_buffer* head, int seqno){
	struct send_data_list_buffer* tmp;
	if(head==NULL)
		return NULL;
	tmp= head;
	do{
		if(ntohl(tmp->packet->header.seqno) == seqno){
			return tmp;
		}
		tmp= tmp->next;	
	}while(tmp!=NULL);
	return NULL;
}

int send_ack(struct rudp_socket* skt, struct sockaddr_in* dest, int seqnum){
	int ret = 0;
	rudp_packet* packet;
	packet = createRUDPPacket(RUDP_ACK, seqnum, NULL, 0);
	ret = sendto((int)skt->fd, (void*)packet, sizeof(struct rudp_hdr), 0,
				(struct sockaddr*)dest, sizeof(struct sockaddr_in));
	if(ret <= 0){
		fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
		return -1;
	}
	return 0;
}

int send_data(struct rudp_socket *skt, struct sockaddr_in *dest){
	int ret;
	struct send_data_list_buffer* node;
	struct timeval t , t1, t2;
	while(skt->window_size>0){
		node = findNode(skt->head, skt->hack+RUDP_WINDOW-skt->window_size);
		if(node == NULL){
			return -1;
		}
		node->fd = skt->fd;
		if(ntohs(node->packet->header.type) == RUDP_FIN && skt->reachedEnd == 0){
			skt->reachedEnd = 1;	
			return 2;// to know its a fin
		}
		ret = sendto((int)skt->fd, (void *)node->packet, node->datalen+sizeof(struct rudp_hdr), 0,
				(struct sockaddr*)dest, sizeof(struct sockaddr_in));
		if(ret <= 0){
			fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
			return -1;
		}
		t.tv_sec = RUDP_TIMEOUT/1000;           	// Convert to seconds.
		t.tv_usec = (RUDP_TIMEOUT%1000) * 1000; 	// Convert to microseconds.
		gettimeofday(&t1, NULL);     			// Get current time of the day.
		timeradd(&t1, &t, &t2);  			// Sum the timeout time with the current time of the day.
								// Start the timeout callback with event_timeout.
		if(event_timeout(t2, &rudp_retransmit, node, "timer_callback") == -1){
			fprintf(stderr,"Error(event): wasn't able to register event to the eventloop.\n");
			return -1;
		}
		skt->window_size = skt->window_size-1;
	}
	return 0;
}


void handleINITState(struct rudp_socket* skt, rudp_packet* packet, struct sockaddr_in* dest){	
	switch(ntohs(packet->header.type)){
	case RUDP_SYN:
		skt->state = DATA;
		skt->hack = ntohl(packet->header.seqno)+1;
		send_ack(skt, dest, skt->hack);
		break;
	default:
		break;
	}
}

void handleWAITFINACKState(struct rudp_socket* skt, rudp_packet* packet, struct sockaddr_in* dest){
	switch(ntohs(packet->header.type)){
	case RUDP_ACK:
		if(ntohl(packet->header.seqno) == skt->hack+1){
			skt->state = FIN;
			skt->head = removeNode(skt->head);
			skt->event_handler_callback((rudp_socket_t*)skt,RUDP_EVENT_CLOSED,dest);
			event_fd_delete(&rudp_receive_data, (void*)skt);
			close(skt->fd);
			free(skt);			
			fprintf(stdout, "File sending successful!\n");
		}
		break;
	default:
		break;
	}
}

void handleDATAState(struct rudp_socket* skt, rudp_packet* packet, 
		struct sockaddr_in* dest, int datalen){
	switch(ntohs(packet->header.type)){
	case RUDP_DATA:
		if(ntohl(packet->header.seqno) == skt->hack){
			skt->recvfrom_handler_callback((rudp_socket_t*)skt, dest, 
				(char*)packet->data, datalen);
			skt->hack = skt->hack+1;
		}
		send_ack(skt, dest, skt->hack);
		break;
	case RUDP_ACK:
		if(ntohl(packet->header.seqno) == skt->synseqno+1){
			skt->head = removeNode(skt->head);
			skt->hack = skt->hack+1;
			skt->window_size = RUDP_WINDOW;
		}
		if((ntohl(packet->header.seqno) > skt->hack) && 
				(ntohl(packet->header.seqno) <= 
					(skt->hack+RUDP_WINDOW-skt->window_size))){
			do{
				skt->head = removeNode(skt->head);
				skt->hack = skt->hack+1;
				skt->window_size = skt->window_size+1;	
			}while(ntohl(packet->header.seqno) > skt->hack);
		}
		send_data(skt,dest);			
		break;
	case RUDP_FIN:
		if(ntohl(packet->header.seqno) == skt->hack){
			skt->state = FIN;
			skt->event_handler_callback((rudp_socket_t*)skt, RUDP_EVENT_CLOSED, dest);
			skt->hack = skt->hack+1;
			send_ack(skt, dest, skt->hack);
			skt->state = INIT;
			skt->hack = 0;
			skt->seqno = 0;
			skt->window_size = 0;
		}else{
			send_ack(skt, dest, skt->hack);
		}
		break;
	default:
		break;
	
	}
}

void handleCLOSINGState(struct rudp_socket* skt, rudp_packet* packet, 
		struct sockaddr_in* dest,int datalen){
	switch(ntohs(packet->header.type)){
	case RUDP_ACK:
		if(ntohl(packet->header.seqno) == skt->synseqno+1){
			skt->head = removeNode(skt->head);
			skt->hack = skt->hack+1;
			skt->window_size = RUDP_WINDOW;
			send_data(skt, dest);
		}
		if(skt->reachedEnd ==  1){
			if(ntohl(packet->header.seqno) == skt->seqno){
				do{
					skt->head = removeNode(skt->head);
					skt->hack = skt->hack+1;
					skt->window_size = skt->window_size+1;	
				}while(ntohl(packet->header.seqno) > skt->hack);
				send_data(skt, dest);
				skt->state = WAIT_FIN_ACK;
			}else{	
				do{
					skt->head = removeNode(skt->head);
					skt->hack = skt->hack+1;
					skt->window_size = skt->window_size+1;	
				}while(ntohl(packet->header.seqno) > skt->hack);
			}
		}else{
			if((ntohl(packet->header.seqno) > skt->hack) && 
					(ntohl(packet->header.seqno) <= 
						(skt->hack+RUDP_WINDOW-skt->window_size))){
				do{
					skt->head = removeNode(skt->head);
					skt->hack = skt->hack+1;
					skt->window_size = skt->window_size+1;	
				}while(ntohl(packet->header.seqno) > skt->hack);
			}
			send_data(skt,dest);
		}			
		break;
	default:
		break;
	
	}
}

int rudp_receive_data(int fd, void *arg){
	struct rudp_socket* skt = (struct rudp_socket*)arg;
        struct sockaddr_in dest;
	rudp_packet rudp_data;
	int addr_size;
	int bytes;
	memset(&rudp_data, 0x0, sizeof(rudp_packet));
	addr_size = sizeof(struct sockaddr_in);
	bytes = recvfrom((int)fd, (void*)&rudp_data, sizeof(rudp_data), 0, 
		(struct sockaddr*)&dest, (socklen_t*)&addr_size);
	if(bytes <= 0){
		printf("[Error]: recvfrom failed(fd=%d).\n", fd);
		return -1;
	}
	switch(skt->state){
	case INIT:
		handleINITState(skt, &rudp_data, &dest);
		break;
	case DATA:
		handleDATAState(skt, &rudp_data, &dest, bytes-sizeof(struct rudp_hdr));
		break;
	case CLOSING:
		handleCLOSINGState(skt, &rudp_data, &dest, bytes-sizeof(struct rudp_hdr));
		break;
	case WAIT_FIN_ACK:
		handleWAITFINACKState(skt, &rudp_data, &dest);
	case FIN:
		break;
	default:
		break;
	}
	return 0;
}



/* 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 */

rudp_socket_t rudp_socket(int port){
        struct rudp_socket* skt;
	int fd;
	struct sockaddr_in* in;	
	int eventRet;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(fd < 0){
		fprintf(stderr, "rudp: socket error : ");
		return NULL;
	}
        if(port == 0){
		port = rand()%60000 + 4711; 			// Randomize a number between 4711 and 64711.
    	}
    	printf("rudp_socket: Socketfd: %d, Port number: %d\n",fd, port);
        in = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
	bzero(in, sizeof(struct sockaddr_in));			// Reset all values inside the allocated sockadder_in structure.
	in->sin_family = AF_INET;				// Set the socket internet family to be AF_INET.
	in->sin_addr.s_addr = htonl(INADDR_ANY);		// Set the socket address in network byte order.
	in->sin_port = htons(port);				// Set the socket port in network byte order.
	if(bind(fd, (struct sockaddr*)in, sizeof(struct sockaddr_in)) == -1){
		fprintf(stderr, "rudp: bind error\n");
		return NULL;
	}
	free(in);						// Free the allocated space.
        skt = (struct rudp_socket*)malloc(sizeof(struct rudp_socket));
	skt->fd = fd;						// Register the socket file descriptor.
	skt->dest = NULL;					// Set the destination to be NULL.
	skt->state = INIT;					// Make the socket start in the INIT socket state.
	skt->window_size = RUDP_WINDOW;				// Set the socket window size to 3.
	skt->reachedEnd = 0;					// |-(==1): The next packtet to send is RUDP FIN.
								// |-(==0): There are still buffered packets to send.
	eventRet = event_fd((int)fd, &rudp_receive_data, (void*)skt, "rudp_receive_data");
	if(eventRet < 0){
		printf("[Error] event_fd failed: rudp_receive_data()\n");
		return NULL;
	}
	return (rudp_socket_t*)skt;
}

/* 
 *rudp_close: Close socket 
 */ 

int rudp_close(rudp_socket_t rsocket){
	struct rudp_socket* skt;
	struct send_data_list_buffer* node;
	skt = (struct rudp_socket*)rsocket;
	skt->seqno = skt->seqno+1;				// Increment the socket sequence number to be used by the FIN.
	rudp_packet* fin = createRUDPPacket(RUDP_FIN, skt->seqno, NULL, 0);
	node = createNodeBuffer(fin, skt, 0, skt->dest);	// Create a buffer structure for the RUDP FIN packet.
	addNode(skt->head, node);				// Add the RUDP FIN to the buffer list.
	skt->state = CLOSING;					// Set the state of the socket to CLOSING.
	return 0;
}

/* 
 *rudp_recvfrom_handler: Register receive callback function 
 */ 

int rudp_recvfrom_handler(rudp_socket_t rsocket, int (*handler)(rudp_socket_t, 
		struct sockaddr_in *, char *, int)){
	struct rudp_socket *socket =  (struct rudp_socket*)rsocket;
	socket->recvfrom_handler_callback = handler;		
	return 0;	
}

/* 
 *rudp_event_handler: Register event handler callback function 
 */ 
int rudp_event_handler(rudp_socket_t rsocket, int (*handler)(rudp_socket_t, rudp_event_t, 
		struct sockaddr_in *)){
	struct rudp_socket *socket = (struct rudp_socket*)rsocket;
	socket->event_handler_callback = handler;		
	return 0;
}

/* 
 * rudp_sendto: Send a block of data to the receiver. 
 */

int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* dest){
	struct rudp_socket* skt;
	struct send_data_list_buffer* node;
	struct timeval t, t1, t2;
	skt = (struct rudp_socket*)rsocket;
	if(skt->state == CLOSING || skt->state == WAIT_FIN_ACK || skt->state == FIN){
		return -1;
	}else if(skt->state == INIT){
		skt->dest = dest;				// Register the destination address of this socket.
		int seqno = rand()%MAX_SEQ;			// Randomize a integer with modulo 2147483646. 
		rudp_packet* syn = sendSYN(skt->fd, dest, seqno);
		node = createNodeBuffer(syn, skt, len, skt->dest);
		t.tv_sec = RUDP_TIMEOUT/1000;           	// Convert to seconds.
		t.tv_usec = (RUDP_TIMEOUT%1000) * 1000; 	// Convert to microseconds.
		gettimeofday(&t1, NULL);     			// Get current time of the day.
		timeradd(&t1, &t, &t2);  			// Sum the timeout time with the current time of the day.
								// Start the timeout callback with event_timeout.
		if(event_timeout(t2, &rudp_retransmit, node, "timer_callback") == -1){
			fprintf(stderr,"Error(event): wasn't able to register event to the eventloop.\n");
			return -1;
		}
		skt->head = addNode(skt->head, node);		// Initialize the socket head pointer.
		skt->hack = seqno;				// Initialize the socket hack to SYN sequence number + 1;
		skt->seqno = seqno;				// Initialize the sequence number for the following packet.
		skt->synseqno = seqno;				// Register the sequence number of the SYN for later use.
		skt->state = DATA;				// Set the socket state.
	}
	rudp_packet* packet;
	skt->seqno = skt->seqno+1;				// Increment the sequence number for the next packet.
	packet = createRUDPPacket(RUDP_DATA, skt->seqno, (char*)data, len);
	node = createNodeBuffer(packet, skt, len, skt->dest);
	addNode(skt->head, node);			
	return 0;
}


rudp_packet* sendSYN(int fd, struct sockaddr_in* dest, int seqno){
	rudp_packet* packet;
	packet = createRUDPPacket(RUDP_SYN, seqno, NULL, 0);
	if(sendto(fd, (char*)packet, sizeof(struct rudp_hdr), 0, (struct sockaddr*)dest,
			sizeof(struct sockaddr_in)) < 0){
		fprintf(stderr, "Sendto() failed\n");
		return NULL;
	}
	return packet;
}

rudp_packet* createRUDPPacket(u_int16_t type, u_int32_t seqno, char* data, int datalen){
	rudp_packet* packet;
	packet = malloc(sizeof(rudp_packet));
	packet->header = createRUDPHeader(type, seqno);
	memcpy((void*)&(packet->data), (void*)data, datalen);
	return packet;
}

struct rudp_hdr createRUDPHeader(u_int16_t type, u_int32_t seqno){
	struct rudp_hdr header;					// Initialize all RUDP packet header field in network byte order.
	header.version = htons(RUDP_VERSION);			// Initialize the RUDP packet header version field. 
	header.type = htons(type);				// Initialize the RUDP packet header type field.
	header.seqno = htonl(seqno);				// Initialize the RUCP packet header sequence number field.
	return header;
}

int rudp_retransmit(int argc, void* arg){
	struct send_data_list_buffer* node = (struct send_data_list_buffer*)arg;
	struct timeval t, t1, t2;
	if(node->retransCount < RUDP_MAXRETRANS){		// It's still possible to retransmit the packet.
		if(sendto(node->fd, (char*)node->packet, node->datalen+sizeof(struct rudp_hdr), 
				0, (struct sockaddr*)node->dest, sizeof(struct sockaddr_in)) < 0){		
			fprintf(stderr, "Error(retransmission of packet): %s\n", strerror(errno));
			return -1;
		}
		node->retransCount = node->retransCount+1;	// Increment the counter for number of retransmissions for
								// this packet.	
		t.tv_sec = RUDP_TIMEOUT/1000;           	// Convert to seconds.
		t.tv_usec = (RUDP_TIMEOUT%1000) * 1000; 	// Convert to microseconds.
		gettimeofday(&t1, NULL);     			// Get current time of the day.
		timeradd(&t1, &t, &t2);  			// Sum the timeout time with the current time of the day.
								// Start the timeout callback with event_timeout.
		if(event_timeout(t2, &rudp_retransmit, node, "timer_callback") == -1){
			fprintf(stderr,"Error(event): wasn't able to register event to the eventloop.\n");
			return -1;
		}
	}else{							// Call back to application with an RUDP_EVENT_TIMEOUT.
		node->skt->event_handler_callback((rudp_socket_t*)node->skt, RUDP_EVENT_TIMEOUT, node->dest);
	}
	return 0;
}
