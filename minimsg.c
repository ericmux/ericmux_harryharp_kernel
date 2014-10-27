/*
 *  Implementation of minimsgs and miniports.
 */
#include <stdlib.h>
#include <limits.h>

#include "minimsg.h"
#include "hashtable.h"
#include "interrupts.h"  //TODO: protect shared data by disabling interrupts
#include "network.h" 
#include "synch.h"
#include "queue.h"

typedef enum {BOUND, UNBOUND} port_classification;

typedef struct mailbox {
    semaphore_t available_messages_sema;
    queue_t received_messages;
} mailbox;

typedef struct destination_data {
    network_address_t destination_address;
    int destination_port;
} destination_data;

typedef struct miniport
{
    port_classification port_type;
    union {
        // bound ports need destination data
        destination_data *destination_data;
        // while unbound ports need a mailbox
        mailbox *mailbox;
    } port_data;
} miniport; 

// The port numbers assigned to new ports.
int current_bound_port_number;

// The port tables map port numbers to miniport_t's.
// If a port number is in bound_ports_table, then
// it is currently being used.
// NOTE: access to these tables must be protected from
// interrupts!
hashtable_t bound_ports_table;
hashtable_t unbound_ports_table;

// A helper function to get the next available bound port number.
// Returns -1 if no bound ports are available. XXX: possibly change
// TODO: diable interrupts here, right?
int get_next_bound_pn() {
    // We count the number of loops to ensure that we don't check for the 
    // same port number twice.
    // TODO: what to do when there are no available ports? Semaphore maybe?
    // ports are numbered 32768 - 65535
    int num_ports = 65535 - 32767;
    int num_loops = 0;
    while (num_loops < num_ports
           && hashtable_key_exists(bound_ports_table, current_bound_port_number))
    {
        current_bound_port_number++;
        if (current_bound_port_number > 65535) {
            // Rollover from 65535 to 32768
            current_bound_port_number = 32768;
        }
        num_loops++;
    }
    
    if (num_loops >= num_ports) return -1; // no ports were available
    
    return current_bound_port_number; 
}

// Pack a mini_header and return the mini_header_t
// Protocol is assumed to be PROTOCOL_MINIDATAGRAM.
// All port numbers are assumed to be valid.
mini_header_t pack_header(network_address_t source_address, int source_port,
                          network_address_t dest_address, int dest_port)
{
    miniheader_t new_header = (miniheader_t)malloc(sizeof(miniheader));

    new_header->protocol = PROTOCOL_MINIDATAGRAM;
    new_header->source_address = pack_address(source_address);
    new_header->source_port = pack_unsigned_int((unsigned int) source_port);
    new_header->destination_address = pack_address(dest_address);
    new_header->destination_port = pack_unsigned_int((unsigned int) dest_port);

    return new_header;
}

/* performs any required initialization of the minimsg layer.
 */
void
minimsg_initialize()
{
    current_bound_port_number = 32768;
    unavailable_bound_ports = hashtable_create();
}

/* Creates an unbound port for listening. Multiple requests to create the same
 * unbound port should return the same miniport reference. It is the responsibility
 * of the programmer to make sure he does not destroy unbound miniports while they
 * are still in use by other threads -- this would result in undefined behavior.
 * Unbound ports must range from 0 to 32767. If the programmer specifies a port number
 * outside this range, it is considered an error.
 */
miniport_t
miniport_create_unbound(int port_number)
{
    miniport_t new_miniport;
    mailbox *new_mailbox;
    semaphore_t new_available_messages_sema;
    queue_t new_received_messages_q;

    // Check for a bad port_number. Port numbers outside of this range are invalid.
    if (port_number < 0 || port_number > 32767) return NULL;

    // Check if a miniport at this port number has already been created. If so,
    // return that miniport. hashtable_get returns 0 on success and stores the
    // pointer found in the table at the address of the 3rd argument.
    if (hashtable_get(unbound_port_table, port_number, &new_miniport) == 0) {
        return new_miniport; // it's not actually new
    }

    // If we're here, then we need to create a new miniport.
    // First thing we do is set up the new miniport's mailbox.
    new_available_messages_sema = semaphore_create();
    semaphore_initialize(new_available_messages_sema, 0);
    new_received_messages_q = queue_create();
    
    new_mailbox = (mailbox *)malloc(sizeof(mailbox));
    mailbox->available_messages_sema = new_available_messages_sema;
    mailbox->received_messages_q = new_received_messages_q;

    // Now we initialize the new miniport.
    new_miniport = (miniport_t) malloc(sizeof(miniport));
    new_miniport->port_type = UNBOUND;
    new_miniport->port_data.mailbox = new_mailbox; 

    // Before we return, we store the new miniport in the unbound port table.
    unbound_port_table = hashtable_put(unbound_port_table, port_number, miniport_t); 

    return new_miniport;
}

/* Creates a bound port for use in sending packets. The two parameters, addr and
 * remote_unbound_port_number together specify the remote's listening endpoint.
 * This function should assign bound port numbers incrementally between the range
 * 32768 to 65535. Port numbers should not be reused even if they have been destroyed,
 * unless an overflow occurs (ie. going over the 65535 limit) in which case you should
 * wrap around to 32768 again, incrementally assigning port numbers that are not
 * currently in use.
 */
miniport_t
miniport_create_bound(network_address_t addr, int remote_unbound_port_number)
{
    miniport_t new_miniport;
    destination_data *new_destination_data;
    int bound_port_number;

    // Check for NULL input or a bad remote_unbound_port_number.
    if (addr == NULL 
        || remote_unbound_port_number < 0 || remote_unbound_port_number > 32767) {
        return NULL;
    }

    // The first thing we do is set up the port's destination data.
    new_destination_data = (destination data *)malloc(sizeof(destination data));
    network_address_copy(addr, new_destination_data->destination_address);
    destination_data->destination_port = remote_unbound_port_number;

    // Now we initialize the new miniport.
    new_miniport = (miniport_t)malloc(sizeof(miniport));
    new_miniport->port_type = BOUND
    new_miniport->port_data.destination_data = new_destination_data;

    // Before we return, we get a bound port number and put the port in the bound port table.
    // This also makes the port number unavailable to other miniports.
    bound_port_number = get_next_bound_pn();
    bound_port_table = hashtable_put(bound_port_table, bound_port_number);

    return new_miniport;
}

/* Destroys a miniport and frees up its resources. If the miniport was in use at
 * the time it was destroyed, subsequent behavior is undefined.
 */
void
miniport_destroy(miniport_t miniport)
{
    // Check for NULL input.
    if (miniport == NULL) return;

    if (miniport->port_type == UNBOUND) {
        semaphore_destore(miniport->port_data.mailbox->available_messages_sema);
        queue_free(miniport->port_data.mailbox->received_messages_q);
        // TODO: remove from unbound ports table, but check for others using pn - not dire but should be done    
    } else {
        // XXX: what to do about destination address?
        // TODO: remove from bound ports table - this is important!!
    }  

    free(miniport);
}

/* Sends a message through a locally bound port (the bound port already has an associated
 * receiver address so it is sufficient to just supply the bound port number). In order
 * for the remote system to correctly create a bound port for replies back to the sending
 * system, it needs to know the sender's listening port (specified by local_unbound_port).
 * The msg parameter is a pointer to a data payload that the user wishes to send and does not
 * include a network header; your implementation of minimsg_send must construct the header
 * before calling network_send_pkt(). The return value of this function is the number of
 * data payload bytes sent not inclusive of the header.
 */
int
minimsg_send(miniport_t local_unbound_port, miniport_t local_bound_port, minimsg_t msg, int len)
{
    char *msg_header;

    // Check for NULL inputs.
    if (local_unbound_port == NULL || local_bound_port == NULL || msg == NULL) {
        return 0;
    }

    // If any of the input arguments are incorrect, the message does not get sent.
    if (local_unbound_port->port_type != UNBOUND 
        || local_bound_port->port_type != BOUND
        || len > MAX_NETWORK_PACKET_SIZE) {
        return 0;
    }

    // Pack our header using data from the local bound port.
    // TODO: somehow need to get PN from local_unbound_port for source port
    // TODO: use network_get_my_address to get source address
    // XXX: this casting seems weird...
    msg_header = (char *)pack_header(,
                                     , 
                                     local_bound_port->port_data.destination_data->destination_address,
                                     local_bound_port->port_data.destination_data->destination_port);

    return 0;
}

/* Receives a message through a locally unbound port. Threads that call this function are
 * blocked until a message arrives. Upon arrival of each message, the function must create
 * a new bound port that targets the sender's address and listening port, so that use of
 * this created bound port results in replying directly back to the sender. It is the
 * responsibility of this function to strip off and parse the header before returning the
 * data payload and data length via the respective msg and len parameter. The return value
 * of this function is the number of data payload bytes received not inclusive of the header.
 */
int minimsg_receive(miniport_t local_unbound_port, miniport_t* new_local_bound_port, minimsg_t msg, int *len)
{
    return 0;
}

