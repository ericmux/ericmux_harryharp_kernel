#include "minisocket.h"


mini_header_reliable_t 
minisocket_utils_pack_reliable_header(network_address_t source_address, int source_port,
					 network_address_t destination_address, int destination_port,
					 char message_type, int seq_number, int ack_number)
{
	mini_header_reliable_t new_header = 
	(mini_header_reliable_t) malloc(sizeof(struct mini_header_reliable));

	new_header->protocol = PROTOCOL_MINISTREAM;
    pack_address(new_header->source_address, source_address);
    pack_unsigned_short(new_header->source_port, (short) source_port);
    pack_address(new_header->destination_address, destination_address);
    pack_unsigned_short(new_header->destination_port, (short) destination_port);
    new_header->message_type = message_type;
    pack_unsigned_int(new_header->seq_number, seq_number);
    pack_unsigned_int(new_header->ack_number, ack_number);

    return new_header;
}

void minisocket_utils_unpack_reliable_header(char *packet_buffer, socket_channel_t *destination_channel,
							socket_channel_t *source_channel, char *msg_type, int *seq_number, int *ack_number)
{
	// A temporary structure to make the implementation below more clear.
	mini_header_reliable_t header = (mini_header_reliable_t) packet_buffer;

	unpack_address(header->source_address, source_channel->address);
	source_channel->port_number = unpack_unsigned_short(header->source_port);
	unpack_address(header->destination_address, destination_channel->address);
	destination_channel->port_number = unpack_unsigned_short(header->destination_port);

	*msg_type = header->message_type;
	*seq_number = unpack_unsigned_int(header->seq_number);
	*ack_number = unpack_unsigned_int(header->ack_number);
}

/* Copies the payload into the memory location specified. It is assumed that the 
 * given memory location is valid and has enough room.
 */
void minisocket_utils_copy_payload(char *location_to_copy_to, char *buffer, int bytes_to_copy)
{
	char *payload = &buffer[sizeof(struct mini_header_reliable)];
	memcpy(location_to_copy_to, payload, bytes_to_copy);
	return;
}

/* Sets a socket's state to closed. Used as an alarm handler. */
void minisocket_utils_close_socket(void *socket_ptr) {
        minisocket_t socket = (minisocket_t) socket;
        socket->state = CONNECTION_CLOSED;
}

/* Waits for the given ACK to come in by calling P on the ACM semaphore. 
 * Returns 1 if the ACK was received and 0 if a timeout occurred.
 */
int minisocket_utils_wait_for_ack(minisocket_t waiting_socket, int timeout_to_wait)
{

	/* A wrapper function to pass into an alarm. */
	void semaphore_V_wrapper(void *semaphore_ptr) {
	        semaphore_t semaphore = (semaphore_t) semaphore_ptr;
	        semaphore_V(semaphore);
	}

	interrupt_level_t old_level;

	// Schedule the timeout alarm. This will wake us up from the P after
	// timeout_to_wait milliseconds if we have not woken up already.
	alarm_id timeout_alarm = register_alarm(timeout_to_wait,
								   			semaphore_V_wrapper,
								   			waiting_socket->ack_sema);


	// Check for ACKs by calling P on the ACK semaphore.
	semaphore_P(waiting_socket->ack_sema);

	// If an ACK was received, then deregister the alarm and return success.
	if (waiting_socket->ack_received) {
		old_level = set_interrupt_level(DISABLED);
		deregister_alarm(timeout_alarm);
		set_interrupt_level(old_level);
		return 1;
	}

	return 0;
}

/* Sends a packet and waits INITIAL_TIMEOUT_MS milliseconds for an ACK. If no 
 * ACK is received within that time, the packet is resent up to MAX_NUM_TIMEOUTS
 * times. Upon each resending of the packet, the time to wait doubles.
 * Returns the number of bytes sent on success and -1 on error.
 */
int minisocket_utils_send_packet_and_wait(minisocket_t sending_socket, int hdr_len, char* hdr,
				  		 int data_len, char* data)
{
	// TODO: implement fragmentation, timeouts.
	int bytes_sent;
	int ack_received;

	int timeout_to_wait = INITIAL_TIMEOUT_MS;
	int num_timeouts = 0;

	while (num_timeouts < MAX_NUM_TIMEOUTS) {
		// Send the packet.
		bytes_sent  = network_send_pkt(sending_socket->destination_channel.address, hdr_len, hdr, data_len, data);
		
		// Wait for an ACK. This function will return 0 if the alarm
		// goes off before an ACK is received.
		ack_received = minisocket_utils_wait_for_ack(sending_socket, timeout_to_wait);

		if (ack_received) {
			// Success! Return the number of bytes.
			return bytes_sent;
		}

		if (!ack_received && sending_socket->state == CONNECTION_CLOSING) {
			// While we were waiting, the connection state changed to closing,
			// exit the function with failure.
			return -1;
		}

		// Otherwise, we received a timeout, so respond accordingly.
		timeout_to_wait = timeout_to_wait * 2;
		num_timeouts++;
	}

	return -1;
}

/* Used for sending control packets, when we don't need to wait for an ACK. */
void minisocket_utils_send_packet_no_wait(minisocket_t sending_socket, char msg_type){
	mini_header_reliable_t header;

	network_address_t source_address;
	network_address_t destination_address; 


	int destination_port = sending_socket->destination_channel.port_number;
	int source_port = sending_socket->listening_channel.port_number;
	int seq_number = sending_socket->seq_number;
	int ack_number = sending_socket->ack_number;

	network_address_copy(sending_socket->destination_channel.address, destination_address);
	network_address_copy(sending_socket->listening_channel.address, source_address);

	header = minisocket_utils_pack_reliable_header(destination_address, destination_port, source_address, source_port,
						 msg_type, seq_number, ack_number);
	// XXX: what happens when &data = NULL in network_send_pkt?
	network_send_pkt(destination_address, sizeof(header), (char *) header, 0, NULL);
}

/* Sets the server's state to OPEN_SERVER and waits for a SYN from a client.
 * When a SYN is received, the server goes to the HANDSHAKING state and
 * responds with a SYNACK. If an ACK is received from the client after
 * this, then the server's state is set to OPEN_CONNECTION and this function
 * returns. If no ACK is received, then the server goes back to waiting
 * for a SYN.
 */ 
void minisocket_utils_wait_for_client(minisocket_t server, minisocket_error *error) {
	int bytes_received;
	int bytes_sent;
	// header is used for both receiving and sending control packets.
	mini_header_reliable_t header;


	server->state = OPEN_SERVER;

	while(1) {
		// First we wait for a SYN.
		while (!server->state == HANDSHAKING) {
			bytes_received = minisocket_receive(
				server,
				(minimsg_t) header,
				0,
				error);

			if (bytes_received == -1) {
				// error will be set by minisocket_receive
				// TODO: return some indication of error.
				// What is a receive error?? What is proper behavior here?
				continue;
			}

			// Check to see if the message received was a SYN.
			if (header->message_type == MSG_SYN) {
				socket_channel_t dummy_source;
				int seq_number;
				int ack_number;

				minisocket_utils_unpack_reliable_header((char * ) header, 
					&server->destination_channel, &dummy_source, (char * ) &dummy_source,
					&seq_number, &ack_number);
				
				server->seq_number++;
				server->ack_number++;
				server->state = HANDSHAKING;
			}
		}

		// We received a SYN, so send a SYNACK back.
		header = minisocket_utils_pack_reliable_header(
			server->destination_channel.address, server->destination_channel.port_number,
			server->listening_channel.address, server->listening_channel.port_number,
			MSG_SYNACK, server->seq_number, server->ack_number);
		server->state = SENDING;
		bytes_sent = minisocket_utils_send_packet_and_wait(server, sizeof(header), (char *) header, 0, NULL);

		if (bytes_sent != sizeof(struct mini_header_reliable)) {
			// We did not receive an ACK to our SYNACK, so go back to
			// waiting for clients.
			network_address_blankify(server->destination_channel.address);
			server->destination_channel.port_number = -1;
			server->state = OPEN_SERVER;
			continue;
		} else {
			// We received an ACK, so a connection has been established.
			server->state = OPEN_CONNECTION;
			return;
		}

	}
}


/*
* Grab an open port for a new client, otherwise returns 0.
*/
int minisocket_utils_client_get_valid_port(){
	int valid_port = current_client_port_index;

	for(; valid_port < MAX_CLIENT_PORT_NUMBER + 1; valid_port++){
		if(current_sockets[valid_port] == NULL) break;
	}
	if(valid_port == MAX_CLIENT_PORT_NUMBER + 1){
		valid_port = MIN_CLIENT_PORT_NUMBER;
		for(; valid_port < current_client_port_index; valid_port++){
			if(current_sockets[valid_port] == NULL) break;
		}
		if(valid_port == current_client_port_index) valid_port = 0;
	}

	return valid_port;
}