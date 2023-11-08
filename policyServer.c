/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

// DPDK library
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_pdump.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <sqlite3.h>
#include <unistd.h> // Include for access() function
#include <sys/stat.h>

#define RTE_TCP_RST 0x04
#define RX_RING_SIZE 2048
#define TX_RING_SIZE 1024

// Define the statistics file name
#define STAT_FILE "stats/stats"
#define STAT_FILE_EXT ".csv"

#define HTTP_GET_METHOD "GET"
#define HTTP_HOST_HEADER "Host: "

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define CACHE_SIZE 1000 // Adjust the cache size as needed
static sqlite3 *db;
static const char *db_path = "/home/dpdk/policy.db";
struct IP_Cache
{
    char ip[INET_ADDRSTRLEN];
    bool exists;
};
static struct IP_Cache ip_cache[CACHE_SIZE];
/* basicfwd.c: Basic DPDK skeleton forwarding example. */

static volatile bool force_quit;

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */

// Timer period for statistics
static uint16_t timer_period = 1;       // 100 Cycle
static uint16_t timer_period_stats = 1; // 1 second
static uint16_t timer_period_send = 1;  // 10 minutes

// TDOO: Create struct for packet broker identifier
// Port statistic struct
struct port_statistics_data
{
    uint64_t tx_count;
    uint64_t rx_count;
    uint64_t tx_size;
    uint64_t rx_size;
    uint64_t dropped;
    uint64_t rstClient;
    uint64_t rstServer;
    // TODO: add size of packet, throughpout.
} __rte_cache_aligned;
struct port_statistics_data port_statistics[RTE_MAX_ETHPORTS];

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
    if (retval != 0)
    {
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
    for (q = 0; q < rx_rings; q++)
    {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++)
    {
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

static FILE *open_file(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (f == NULL)
    {
        printf("Error opening file!\n");
        exit(1);
    }
    return f;
}

static void print_stats_csv_header(FILE *f)
{
    fprintf(f, "ps_id,rst_client,rst_server,rx_count,tx_count,rx_size,tx_size,time,throughput\n"); // Header row
}

static void print_stats_csv(FILE *f, char *timestamp)
{
    // Write data to the CSV file
    fprintf(f, "%d,%ld,%ld,%ld,%ld,%ld,%ld,%s,%d\n", 1, port_statistics[1].rstClient, port_statistics[1].rstServer, port_statistics[0].rx_count, port_statistics[1].tx_count, port_statistics[0].rx_size, port_statistics[1].tx_size, timestamp, 0);
}

static void clear_stats(void)
{
    memset(port_statistics, 0, RTE_MAX_ETHPORTS * sizeof(struct port_statistics_data));
}

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        printf("\n\nSignal %d received, preparing to exit...\n",
               signum);
        force_quit = true;
    }
}

static void
print_stats(void)
{
    uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
    unsigned int portid;

    total_packets_dropped = 0;
    total_packets_tx = 0;
    total_packets_rx = 0;

    const char clr[] = {27, '[', '2', 'J', '\0'};
    const char topLeft[] = {27, '[', '1', ';', '1', 'H', '\0'};

    // Clear screen and move to top left
    printf("%s%s", clr, topLeft);

    printf("\nRefreshed every %d seconds. "
           "Send every %d minutes.\n",
           timer_period_stats, timer_period_send);
    printf("\nPort statistics ====================================");

    for (portid = 0; portid < 2; portid++)
    {
        printf("\nStatistics for port %u ------------------------------"
               "\nPackets sent count: %18" PRIu64
               "\nPackets sent size: %19" PRIu64
               "\nPackets received count: %14" PRIu64
               "\nPackets received size: %15" PRIu64
               "\nPackets dropped: %21" PRIu64
               "\nRST to Client:  %22" PRIu64
               "\nRST to Server:          %14" PRIu64,
               portid,
               port_statistics[portid].tx_count,
               port_statistics[portid].tx_size,
               port_statistics[portid].rx_count,
               port_statistics[portid].rx_size,
               port_statistics[portid].dropped,
               port_statistics[portid].rstClient,
               port_statistics[portid].rstServer);

        total_packets_dropped += port_statistics[portid].dropped;
        total_packets_tx += port_statistics[portid].tx_count;
        total_packets_rx += port_statistics[portid].rx_count;
    }
    printf("\nAggregate statistics ==============================="
           "\nTotal packets sent: %18" PRIu64
           "\nTotal packets received: %14" PRIu64
           "\nTotal packets dropped: %15" PRIu64,
           total_packets_tx,
           total_packets_rx,
           total_packets_dropped);
    printf("\n====================================================\n");

    fflush(stdout);
}

static inline const char* extractDomainfromHTTPS(struct rte_mbuf *pkt)
{
    // Extract Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        printf("Packet is not an IPv4 packet\n");
        return NULL;
    }

    // Extract IPv4 header
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ip_hdr->next_proto_id != IPPROTO_TCP) {
        printf("Packet is not a TCP packet\n");
        return NULL;
    }

    // Extract TCP header
    struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + sizeof(struct rte_ipv4_hdr));

    // Calculate the offset to the TLS header (if TLS is in use)
    int tls_offset = (tcp_hdr->data_off & 0xf0) >> 2;

    if (tls_offset <= 0) {
        printf("No TLS header found in the packet\n");
        return NULL;
    }

    // Calculate the total length of the TLS payload
    int tls_payload_length = ntohs(ip_hdr->total_length) - (sizeof(struct rte_ipv4_hdr) + (tcp_hdr->data_off >> 4) * 4);

    if (tls_payload_length <= 0) {
        printf("No TLS payload found in the packet\n");
        return NULL;
    }

    int start_offset = 76;
    int end_offset = 77;

    if (start_offset < 0 || end_offset >= tls_payload_length) {
        printf("Invalid byte range specified for the TLS payload\n");
        return NULL;
    }

    // Extract the TLS payload as a pointer to uint8_t
    uint8_t *tls_payload = (uint8_t *)tcp_hdr + tls_offset;
    uint16_t combinedValue = (uint16_t)tls_payload[start_offset] << 8 | (uint16_t)tls_payload[end_offset];

    // Process the specific range of bytes in the TLS payload
    int counter = 82 + combinedValue;
    char extractedName[256]; // Assuming a maximum name length of 256 characters
    int nameIndex = 0; // Index for the extractedName array

    while (1) {
        uint16_t type = (uint16_t)tls_payload[counter] << 8 | (uint16_t)tls_payload[counter + 1];

        if (type == 0) {
            uint16_t namelength = (uint16_t)tls_payload[counter + 7] << 8 | (uint16_t)tls_payload[counter + 8];

            for (int i = 0; i < namelength; i++) {
                extractedName[nameIndex] = (char)tls_payload[counter + 9 + i];
                nameIndex++;
            }
            extractedName[nameIndex] = '\0'; // Null-terminate the string
            printf("Name Length: %d\n", namelength);
            printf("Extracted Name: %s\n", extractedName);
            return extractedName;
        } else {
            uint16_t length = (uint16_t)tls_payload[counter + 2] << 8 | (uint16_t)tls_payload[counter + 3];
            counter += length + 4;
        }
    }
}






// Function to extract the HTTP host from the packet
static inline const char* extractDomainfromHTTP(struct rte_mbuf *pkt)
{
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        printf("Not an IPv4 Packet\n");
        return NULL;
    }

    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ipv4_hdr->next_proto_id != IPPROTO_TCP) {
        printf("Not a TCP Packet\n");
        return NULL;
    }

    struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));

    // Calculate the offset to the HTTP payload
    int payload_offset = ((tcp_hdr->data_off & 0xf0) >> 2);

    if (payload_offset <= 0) {
        printf("No HTTP payload found in the packet\n");
        return NULL;
    }

    // Pointer to the HTTP payload
    char *payload = (char *)tcp_hdr + payload_offset;
    
    char *host_start = strstr(payload, "Host:");
    if (host_start != NULL) {
        char *host_end = strchr(host_start, '\r');
        if (host_end != NULL) {
            // Extract the HTTP host.
            char host[256]; // Assuming a reasonable max size for the host.
            int host_length = host_end - host_start - 6; // Subtract "Host: "
            if (host_length > 0 && host_length < 256) {
                strncpy(host, host_start + 6, host_length);
                host[host_length] = '\0'; // Null-terminate the string
                printf("HTTP Host: %s\n", host);
                return host;
            }
        }
    }

    return NULL; // Return NULL if the HTTP host is not found or an error occurs.
}



// void extract_and_print_tls_sni_extension(struct rte_mbuf *mbuf) {
//     // Assuming mbuf contains the TLS packet data
//     uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);

//     struct tls_handshake_message *handshake_msg = (struct tls_handshake_message *)data;

//     // Check if it's a TLS handshake message
//     if (handshake_msg->msg_type == 0x16) {
//         uint16_t msg_length = rte_be_to_cpu_16(*(uint16_t *)&handshake_msg->length);

//         // Check if it's a ServerHello message (type 0x02)
//         if (data[5] == 0x02) {
//             // This is a ServerHello message

//             // Extract and print SNI extension
//             uint8_t *ptr = data + 7;  // Start of ServerHello message
//             uint16_t remaining_length = msg_length - 4;  // Exclude 4-byte message header

//             while (remaining_length > 0) {
//                 struct tls_sni_extension *sni_extension = (struct tls_sni_extension *)ptr;
//                 uint16_t extension_length = rte_be_to_cpu_16(*(uint16_t *)&sni_extension->length);

//                 if (sni_extension->type == 0x00) {
//                     // SNI extension type 0x00 indicates the hostname extension
//                     printf("Server Name Indication (SNI): %.*s\n", extension_length, ptr + 5);
//                     break;  // You may want to handle multiple extensions
//                 }

//                 // Move to the next extension
//                 ptr += extension_length + 5;
//                 remaining_length -= (extension_length + 5);
//             }
//         }
//     }
// }

/* >8 End of main functional part of port initialization. */
static inline void reset_tcp_client(struct rte_mbuf *rx_pkt)
{
    // Extract Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

    // Check if it's an IPv4 packet
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
    {
        // Extract IP header
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

        // Extract TCP header
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

        // Swap source and destination MAC addresses
        struct rte_ether_addr tmp_mac;
        rte_ether_addr_copy(&eth_hdr->dst_addr, &tmp_mac);
        rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&tmp_mac, &eth_hdr->src_addr);

        // Swap source and destination IP addresses
        uint32_t tmp_ip = ip_hdr->src_addr;
        ip_hdr->src_addr = ip_hdr->dst_addr;
        ip_hdr->dst_addr = tmp_ip;

        // Swap source and destination TCP ports
        uint16_t tmp_port = tcp_hdr->src_port;
        tcp_hdr->src_port = tcp_hdr->dst_port;
        tcp_hdr->dst_port = tmp_port;

        // Set TCP flags to reset (RST)
        tcp_hdr->tcp_flags = RTE_TCP_RST;

        // Set the total length of the IP header
        ip_hdr->total_length = rte_cpu_to_be_16(40);

        // Extract the acknowledgment number from the TCP header
        uint32_t ack_number = rte_be_to_cpu_32(tcp_hdr->recv_ack);

        // Set the sequence number in the TCP header to the received acknowledgment number
        tcp_hdr->sent_seq = rte_cpu_to_be_32(ack_number);

        // Reset the acknowledgment number in the TCP header
        tcp_hdr->recv_ack = 0;

        // Calculate and set the new IP and TCP checksums (optional)
        ip_hdr->hdr_checksum = 0;
        tcp_hdr->cksum = 0;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
    }
}

static inline void reset_tcp_server(struct rte_mbuf *rx_pkt)
{
    // Extract Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

    // Check if it's an IPv4 packet
    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
    {
        // Extract IP header
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

        // Extract TCP header
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

        // Increment the TCP sequence number
        uint32_t sequence_number = rte_be_to_cpu_32(tcp_hdr->sent_seq);
        sequence_number++;
        tcp_hdr->sent_seq = rte_cpu_to_be_32(sequence_number);

        // Set the acknowledgment number in the TCP header to the updated sequence number
        tcp_hdr->recv_ack = tcp_hdr->sent_seq;

        // Set TCP flags to reset (RST)
        tcp_hdr->tcp_flags = RTE_TCP_RST;

        // Set the total length of the IP header (if needed)
        ip_hdr->total_length = rte_cpu_to_be_16(40);

        // Calculate and set the new IP and TCP checksums (optional)
        ip_hdr->hdr_checksum = 0;
        tcp_hdr->cksum = 0;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
        tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
    }
}

void init_database()
{
    // Check if the database file exists
    if (access(db_path, F_OK) != -1)
    {
        // Database file exists, open it
        if (sqlite3_open(db_path, &db) != SQLITE_OK)
        {
            // Handle database opening error
            printf("Error opening the database: %s\n", sqlite3_errmsg(db));
            // You may want to exit or return an error code here
        }
    }
    else
    {
        // Database file does not exist, create it
        if (sqlite3_open(db_path, &db) != SQLITE_OK)
        {
            // Handle database creation error
            printf("Error creating the database: %s\n", sqlite3_errmsg(db));
            // You may want to exit or return an error code here
        }
        else
        {
            // Initialize the database schema if needed
            sqlite3_exec(db, "CREATE TABLE policies (ip_address TEXT)", NULL, 0, NULL);
        }
    }
}

static inline bool database_checker(struct rte_mbuf *rx_pkt)
{
    if (!db)
    {
        // Database is not initialized
        return false;
    }

    // Extract the destination IP address from the received packet (assuming IPv4)
    struct rte_ipv4_hdr *ip_hdr = rte_pktmbuf_mtod_offset(rx_pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    char dest_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip_hdr->dst_addr, dest_ip_str, INET_ADDRSTRLEN);

    // Check the cache first
    for (int i = 0; i < CACHE_SIZE; i++)
    {
        if (ip_cache[i].exists && strcmp(dest_ip_str, ip_cache[i].ip) == 0)
        {
            return true; // IP found in cache
        }
    }

    // Prepare an SQL query to check if the destination IP exists in the database
    char query[256];
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM policies WHERE ip_address = '%s'", dest_ip_str);

    // Execute the SQL query
    sqlite3_stmt *stmt;
    int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

    if (result != SQLITE_OK)
    {
        // Handle query preparation error
        printf("Error preparing SQL query: %s\n", sqlite3_errmsg(db));
        return false;
    }

    // Execute the query and check if the destination IP exists in the database
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int(stmt, 0);
    }

    // Finalize the statement
    sqlite3_finalize(stmt);

    // Cache the result
    for (int i = 0; i < CACHE_SIZE; i++)
    {
        if (!ip_cache[i].exists)
        {
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
static __rte_noreturn void lcore_main(void)
{
    uint16_t port;
    init_database();
    uint64_t timer_tsc = 0;
    int last_run_stat = 0;
    int last_run_file = 0;
    int current_sec;
    char time_str[80];
    char time_str_file[80];
    const char *format = "%Y-%m-%dT%H:%M:%S";
    FILE *f_stat = NULL;
    struct tm *tm_info, *tm_rounded;
    time_t now, rounded;

    /* Check NUMA locality for each port for optimal performance. */
    RTE_ETH_FOREACH_DEV(port)
    {
        if (rte_eth_dev_socket_id(port) >= 0 &&
            rte_eth_dev_socket_id(port) != (int)rte_socket_id())
        {
            printf("WARNING, port %u is on a remote NUMA node to "
                   "the polling thread. Performance may be suboptimal.\n",
                   port);
        }
    }

    printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n", rte_lcore_id());

    /* Main work of the application loop. */
    while (!force_quit)
    {

        /* Get a burst of RX packets from the first port of the pair. */
        struct rte_mbuf *rx_bufs[BURST_SIZE];
        const uint16_t rx_count = rte_eth_rx_burst(0, 0, rx_bufs, BURST_SIZE);

        port_statistics[0].rx_count += rx_count;
        for (uint16_t i = 0; i < rx_count; i++)
        {

            struct rte_mbuf *rx_pkt = rx_bufs[i];
            // extractDomainfromHTTPS(rx_pkt);
            extractDomainfromHTTP(rx_pkt);
            port_statistics[0].rx_size += rte_pktmbuf_pkt_len(rx_pkt);

            if (database_checker(rx_pkt))
            {
                printf("Packet Detected in database\n");

                // Create a copy of the received packet
                struct rte_mbuf *rst_pkt_client = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
                if (rst_pkt_client == NULL)
                {
                    printf("Error copying packet to RST Client\n");
                    rte_pktmbuf_free(rx_pkt); // Free the original packet                // Skip this packet
                }
                struct rte_mbuf *rst_pkt_server = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
                if (rst_pkt_server == NULL)
                {
                    printf("Error copying packet to RST Server\n");
                    rte_pktmbuf_free(rx_pkt); // Free the original packet
                }

                // Apply modifications to the packets
                reset_tcp_client(rst_pkt_client);
                reset_tcp_server(rst_pkt_server);

                // Transmit modified packets
                const uint16_t rst_client_tx_count = rte_eth_tx_burst(1, 0, &rst_pkt_client, 1);
                if (rst_client_tx_count == 0)
                {
                    printf("Error sending packet to client\n");
                    port_statistics[1].dropped++;
                    rte_pktmbuf_free(rst_pkt_client); // Free the modified packet
                }
                else
                {
                    port_statistics[1].rstClient++;
                    port_statistics[1].tx_size += rte_pktmbuf_pkt_len(rst_pkt_client);
                    port_statistics[1].tx_count += rst_client_tx_count;
                    printf("Packet to client sent\n");
                }

                const uint16_t rst_server_tx_count = rte_eth_tx_burst(1, 0, &rst_pkt_server, 1);
                if (rst_server_tx_count == 0)
                {
                    printf("Error sending packet to server\n");
                    port_statistics[1].dropped++;
                    rte_pktmbuf_free(rst_pkt_server); // Free the modified packet
                }
                else
                {
                    port_statistics[1].rstServer++;
                    port_statistics[1].tx_size += rte_pktmbuf_pkt_len(rst_pkt_server);
                    port_statistics[1].tx_count += rst_server_tx_count;
                    printf("Packet to Server sent\n");
                }
            }
            rte_pktmbuf_free(rx_pkt); // Free the original packet
            port_statistics[0].dropped++;

            // if (timer_period > 0)
            // {
            //     /* Advance the timer */
            //     timer_tsc++;

            //     /* If the timer has reached its timeout */
            //     if (timer_tsc >= timer_period)
            //     {
            //         /* Do this only on the main core */
            //         if (rte_lcore_id() == rte_get_main_lcore())
            //         {
            //             print_stats();
            //         }
            //         /* Reset the timer */
            //         timer_tsc = 0;
            //     }
            // }
        }

        rte_pktmbuf_free(*rx_bufs);

        /* If the timer is enabled */

        // // // Print Statistcs to file
        time(&now);
        tm_info = localtime(&now);
        current_sec = tm_info->tm_sec;
        if (current_sec % timer_period_stats == 0 && current_sec != last_run_stat)
        {
            char *filename = (char *)calloc(100, 100);

            // get the current minute
            int current_min = tm_info->tm_min;

            // check file
            if (!f_stat)
            {
                int remaining_seconds = current_min % timer_period_send * 60 + current_sec;
                rounded = now - remaining_seconds;
                tm_rounded = localtime(&rounded);
                strftime(time_str_file, sizeof(time_str_file), format, tm_rounded);
                strcat(filename, STAT_FILE);
                strcat(filename, time_str_file);
                strcat(filename, STAT_FILE_EXT);
                f_stat = open_file(filename);
                // print the header of the statistics file
                print_stats_csv_header(f_stat);
                // free the string
                free(filename);
                last_run_file = tm_rounded->tm_min;
            }

            // convert the time to string
            strftime(time_str, sizeof(time_str), format, tm_info);

            // print out the stats to csv
            print_stats_csv(f_stat, time_str);
            fflush(f_stat);

            // clear the stats
            clear_stats();

            if (current_min % timer_period_send == 0 && current_min != last_run_file)
            {
                strcat(filename, STAT_FILE);
                strcat(filename, time_str);
                strcat(filename, STAT_FILE_EXT);
                f_stat = open_file(filename);

                // print the header of the statistics file
                print_stats_csv_header(f_stat);

                // free the string
                free(filename);

                // set the last run file
                last_run_file = current_min;
            }

            // Set the last run time
            last_run_stat = current_sec;
        }
    }
}

/* >8 End Basic forwarding application lcore. */

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */

int main(int argc, char *argv[])
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

    // force quit handler
    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // // Clear the statistics
    // memset(port_statistics, 0, 32 * sizeof(struct port_statistics_data));

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
        rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n",
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
