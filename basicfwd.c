/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <arpa/inet.h> 
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_tcp.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h> // Include for access() function
#include <sys/stat.h> // Include for stat() function


#define RTE_TCP_RST 0x04
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define CACHE_SIZE 1000 // Adjust the cache size as needed
static sqlite3 *db; 
static const char *db_path = "/home/dpdk/policy.db";
struct IP_Cache {
    char ip[INET_ADDRSTRLEN];
    bool exists;
};
static struct IP_Cache ip_cache[CACHE_SIZE];
/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */

/* Main functional part of port initialization. 8< */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Starting Ethernet port. 8< */
	retval = rte_eth_dev_start(port);
	/* >8 End of starting of ethernet port. */
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, RTE_ETHER_ADDR_BYTES(&addr));

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	/* End of setting RX port in promiscuous mode. */
	if (retval != 0)
		return retval;

	return 0;
}
/* >8 End of main functional part of port initialization. */




static inline void modify_ip_tcp_headers(struct rte_mbuf *rx_pkt) {
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

    // Check if it's an IPv4 packet
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

        // Swap MAC addresses
        struct rte_ether_addr tmp_mac;
        rte_ether_addr_copy(&eth_hdr->dst_addr, &tmp_mac);
        rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&tmp_mac, &eth_hdr->src_addr);

        // Swap IP addresses
        uint32_t tmp_ip = ip_hdr->src_addr;
        ip_hdr->src_addr = ip_hdr->dst_addr;
        ip_hdr->dst_addr = tmp_ip;

        // Swap TCP ports
        uint16_t tmp_port = tcp_hdr->src_port;
        tcp_hdr->src_port = tcp_hdr->dst_port;
        tcp_hdr->dst_port = tmp_port;

        // Set TCP flags to reset (RST)
        tcp_hdr->tcp_flags = RTE_TCP_RST;

		// Calculate TCP payload length
   		uint16_t tcp_payload_len = rte_be_to_cpu_16(ip_hdr->total_length) - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_tcp_hdr);
		// Point to the TCP payload data
   		// char *tcp_payload = (char *)tcp_hdr + sizeof(struct rte_tcp_hdr);
        // Trim the packet length to the new size
        rte_pktmbuf_trim(rx_pkt, tcp_payload_len);
		
		ip_hdr->total_length = rte_cpu_to_be_16(40);
		
		// Extract the acknowledgment number from the TCP header
		uint32_t ack_number = rte_be_to_cpu_32(tcp_hdr->recv_ack);

		// Set the sequence number in the TCP header to the received acknowledgment number
		tcp_hdr->sent_seq = rte_cpu_to_be_32(ack_number);

		tcp_hdr->recv_ack = 0;

        // Calculate and set the new IP and TCP checksums (optional)
        ip_hdr->hdr_checksum = 0;
        tcp_hdr->cksum = 0;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);

        // Extract TCP flags
        uint16_t tcp_flags = rte_be_to_cpu_16(tcp_hdr->tcp_flags);

        // // Calculate and store the packet size
        // uint16_t packet_size = rx_pkt->pkt_len;

        // // Print modified packet information (assuming you have a debug log)
        // char src_ip_str[INET_ADDRSTRLEN];
        // char dst_ip_str[INET_ADDRSTRLEN];
        // inet_ntop(AF_INET, &ip_hdr->src_addr, src_ip_str, INET_ADDRSTRLEN);
        // inet_ntop(AF_INET, &ip_hdr->dst_addr, dst_ip_str, INET_ADDRSTRLEN);

        // struct rte_ether_addr *src_mac = &eth_hdr->src_addr;
        // struct rte_ether_addr *dst_mac = &eth_hdr->dst_addr;

        // printf("\nModified Packets\n");
        // printf("Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        //        src_mac->addr_bytes[0], src_mac->addr_bytes[1], src_mac->addr_bytes[2],
        //        src_mac->addr_bytes[3], src_mac->addr_bytes[4], src_mac->addr_bytes[5]);
        // printf("Destination MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        //        dst_mac->addr_bytes[0], dst_mac->addr_bytes[1], dst_mac->addr_bytes[2],
        //        dst_mac->addr_bytes[3], dst_mac->addr_bytes[4], dst_mac->addr_bytes[5]);
        // printf("Source IP: %s, Source Port: %u\n", src_ip_str, rte_be_to_cpu_16(tcp_hdr->src_port));
        // printf("Destination IP: %s, Destination Port: %u\n", dst_ip_str, rte_be_to_cpu_16(tcp_hdr->dst_port));
        // printf("TCP Flags: 0x%x\n", tcp_flags);
        // printf("Packet Size: %u bytes\n", packet_size);
    }
}




static inline void view_packet(struct rte_mbuf *pkt) {
    // Assuming it's an Ethernet frame containing an IPv4 packet
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    
    // Check if it's an IPv4 packet
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        // Access the IP Header
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        // Access the TCP header
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

        uint8_t *src_mac = eth_hdr->src_addr.addr_bytes;
        uint8_t *dst_mac = eth_hdr->dst_addr.addr_bytes;

        // Access the source and destination IPv4 addresses
        uint32_t src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
        uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);        

        // Access the source and destination ports
        uint16_t src_port = rte_be_to_cpu_16(tcp_hdr->src_port);
        uint16_t dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);

        // Access the TCP flags
        uint16_t tcp_flags = rte_be_to_cpu_16(tcp_hdr->tcp_flags);

        // Calculate the packet size
        uint16_t packet_size = pkt->pkt_len;

        // Manually reverse the byte order of the IP addresses
        src_ip = (src_ip >> 24) | ((src_ip << 8) & 0xFF0000) | ((src_ip >> 8) & 0xFF00) | (src_ip << 24);
        dst_ip = (dst_ip >> 24) | ((dst_ip << 8) & 0xFF0000) | ((dst_ip >> 8) & 0xFF00) | (dst_ip << 24);
        
        // Convert the numeric IP addresses to dotted-decimal strings
        char src_ip_str[INET_ADDRSTRLEN];
        char dst_ip_str[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &src_ip, src_ip_str, INET_ADDRSTRLEN);
        inet_ntop(AF_INET, &dst_ip, dst_ip_str, INET_ADDRSTRLEN);

        // Print the source and destination IP addresses, ports, TCP flags, and packet size
        // printf("\nReceived Packets\n");
        // printf("Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        //        src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
        // printf("Destination MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        //        dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);
        // printf("Source IP: %s, Source Port: %d\n", src_ip_str, src_port);
        // printf("Destination IP: %s, Destination Port: %d\n", dst_ip_str, dst_port);
        // printf("TCP Flags: 0x%x\n", tcp_flags);
        // printf("Packet Size: %d bytes\n", packet_size);

		printf("Packet Received");
    } else {
        // Not an IPv4 packet, handle accordingly
    }
}

static inline void rst_to_server(struct rte_mbuf *rx_pkt, struct rte_mbuf *new_pkt) {
    // Calculate the size of the new packet
    uint16_t new_pkt_size = rte_pktmbuf_pkt_len(rx_pkt);

    // Allocate a new mbuf for the new packet
    new_pkt = rte_pktmbuf_alloc(rx_pkt->pool);
    if (new_pkt == NULL) {
         printf("Error Allocation New Packet");
        return;
    }

    // Copy the entire packet (headers and payload) to the new packet
    rte_memcpy(rte_pktmbuf_mtod(new_pkt, void *), rte_pktmbuf_mtod(rx_pkt, void *), new_pkt_size);

    // Optionally, modify any header fields in the new packet
    // For example, you can modify the IP addresses, TCP ports, or other fields here
	 struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(new_pkt, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);
    
    uint32_t old_seq_num = rte_be_to_cpu_32(tcp_hdr->sent_seq); // Get old sequence number
    tcp_hdr->sent_seq = rte_cpu_to_be_32(old_seq_num + 1);
	tcp_hdr->recv_ack = rte_cpu_to_be_32(tcp_hdr->sent_seq);

	// Calculate TCP payload length
   		uint16_t tcp_payload_len = rte_be_to_cpu_16(ip_hdr->total_length) - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_tcp_hdr);
		// Point to the TCP payload data
   		// char *tcp_payload = (char *)tcp_hdr + sizeof(struct rte_tcp_hdr);
        // Trim the packet length to the new size
        rte_pktmbuf_trim(new_pkt, tcp_payload_len);
		
		ip_hdr->total_length = rte_cpu_to_be_16(40);
		  // Calculate and set the new IP and TCP checksums (optional)
        ip_hdr->hdr_checksum = 0;
        tcp_hdr->cksum = 0;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);

}

void init_database() {
    // Check if the database file exists
    if (access(db_path, F_OK) != -1) {
        // Database file exists, open it
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            // Handle database opening error
            printf("Error opening the database: %s\n", sqlite3_errmsg(db));
            // You may want to exit or return an error code here
        }
    } else {
        // Database file does not exist, create it
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            // Handle database creation error
            printf("Error creating the database: %s\n", sqlite3_errmsg(db));
            // You may want to exit or return an error code here
        } else {
            // Initialize the database schema if needed
            sqlite3_exec(db, "CREATE TABLE policies (ip_address TEXT)", NULL, 0, NULL);
        }
    }
}



bool is_destination_ip_in_database(struct rte_mbuf *rx_pkt) {
    if (!db) {
        // Database is not initialized
        return false;
    }

    // Extract the destination IP address from the received packet (assuming IPv4)
    struct rte_ipv4_hdr *ip_hdr = rte_pktmbuf_mtod_offset(rx_pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    char dest_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip_hdr->dst_addr, dest_ip_str, INET_ADDRSTRLEN);

    // Check the cache first
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (ip_cache[i].exists && strcmp(dest_ip_str, ip_cache[i].ip) == 0) {
            return true; // IP found in cache
        }
    }

    // Prepare an SQL query to check if the destination IP exists in the database
    char query[256];
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM policies WHERE ip_address = '%s'", dest_ip_str);

    // Execute the SQL query
    sqlite3_stmt *stmt;
    int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

    if (result != SQLITE_OK) {
        // Handle query preparation error
        printf("Error preparing SQL query: %s\n", sqlite3_errmsg(db));
        return false;
    }

    // Execute the query and check if the destination IP exists in the database
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    // Finalize the statement
    sqlite3_finalize(stmt);

    // Cache the result
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!ip_cache[i].exists) {
            strncpy(ip_cache[i].ip, dest_ip_str, INET_ADDRSTRLEN);
            ip_cache[i].exists = (count > 0);
            break;
        }
    }

    return count > 0;
}
/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */

 /* Basic forwarding application lcore. 8< */
static __rte_noreturn void
lcore_main(void)
{
	uint16_t port;
	init_database();
	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
		if (rte_eth_dev_socket_id(port) >= 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());
	
	
	/* Main work of application loop. 8< */
	for (;;) {
        struct rte_mbuf *new_pkt = NULL;
		/*
		 * Receive packets on a port and forward them on the paired
		 * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
		 */
		RTE_ETH_FOREACH_DEV(port) {

			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(0, 0,
					bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0))
				continue;
			else {
				for (uint16_t i = 0; i < nb_rx; i++) {
				struct rte_mbuf *pkt = bufs[i];
				// view_packet(pkt);
				if(is_destination_ip_in_database(pkt)){
					rst_to_server(pkt, new_pkt);
					modify_ip_tcp_headers(pkt);
				}else{
					continue;
				}
				
				}
			}
            if (new_pkt != NULL) {
                        // Modify or process the new packet if needed
                        // Example: modify_ip_tcp_headers(new_pkt);

                        // Forward the new packet (assuming port 1 is the destination)
                        const uint16_t nb_tx = rte_eth_tx_burst(1, 0, &new_pkt, 1);

                        if(nb_tx){
                            printf("packet to server");
                        }

                        // Free any unsent packets
                        if (unlikely(nb_tx == 0)) {
                            rte_pktmbuf_free(new_pkt);
                        }
                    }
			const uint16_t nb_tx = rte_eth_tx_burst(1, 0,
					bufs, nb_rx);

			/* Free any unsent packets. */
			// if (unlikely(nb_tx < nb_rx)) {
			// 	uint16_t buf;
			// 	for (buf = nb_tx; buf < nb_rx; buf++)
			// 		rte_pktmbuf_free(bufs[buf]);
			// }

            if(nb_tx){
                printf("packet to client");
            }

			

		}
	}
	/* >8 End of loop. */
}
/* >8 End Basic forwarding application lcore. */

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */




int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;

	/* Initializion the Environment Abstraction Layer (EAL). 8< */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	/* >8 End of initialization the Environment Abstraction Layer (EAL). */

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */

	/* Allocates mempool to hold the mbufs. 8< */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	/* >8 End of allocating mempool to hold mbuf. */

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initializing all ports. 8< */
	RTE_ETH_FOREACH_DEV(portid)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
					portid);
	/* >8 End of initializing all ports. */

	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the main core only. Called on single lcore. 8< */
	lcore_main();
	/* >8 End of called on single lcore. */
	
	/* clean up the EAL */
	rte_eal_cleanup();
	

	return 0;
}


