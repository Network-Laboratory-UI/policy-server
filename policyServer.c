
// C library
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
#include <rte_cycles.h>
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
#include <unistd.h>
#include <sys/stat.h>

#define CACHE_SIZE 1000
#define RTE_TCP_RST 0x04

uint32_t MAX_PACKET_LEN;
uint32_t RX_RING_SIZE;
uint32_t TX_RING_SIZE;
uint32_t NUM_MBUFS;
uint32_t MBUF_CACHE_SIZE;
uint32_t BURST_SIZE;
uint32_t MAX_TCP_PAYLOAD_LEN;

char STAT_FILE[100];
char STAT_FILE_EXT[100];

static const char *db_path = "/home/ubuntu/policy.db";
static sqlite3 *db;

// Force quit variable
static volatile bool force_quit;

// Timer period for statistics
static uint32_t TIMER_PERIOD;		// in Miliseconds
static uint32_t TIMER_PERIOD_STATS; // 1 second
static uint32_t TIMER_PERIOD_SEND;	// 10 minutes

// TDOO: Create struct for packet broker identifier

// Port statistic struct
struct port_statistics_data
{
	uint64_t tx_count;
	uint64_t rx_count;
	uint64_t tx_size;
	uint64_t rx_size;
	uint64_t dropped;
	uint64_t rstServer;
	uint64_t rstClient;
	uint64_t throughput;
} __rte_cache_aligned;
struct port_statistics_data port_statistics[RTE_MAX_ETHPORTS];

struct IP_Cache
{
	char ip[INET_ADDRSTRLEN];
	bool exists;
};
static struct IP_Cache ip_cache[CACHE_SIZE];

struct rte_eth_stats stats_0;
struct rte_eth_stats stats_1;

// ======================================================= THE FUNCTIONS =======================================================

// PORT INITIALIZATION
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	// Declaration
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	// Check port validity
	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	// Set memory for port configuration
	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	// Get the port info
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

	// Configure the Ethernet device
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	// Allocate 1 Rx queue for each port
	for (q = 0; q < rx_rings; q++)
	{
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
										rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	// Allocate 1 Tx queue for each port
	for (q = 0; q < tx_rings; q++)
	{
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
										rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	// Starting the ethernet port
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	// Display the MAC Addresses
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
		   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
		   port, RTE_ETHER_ADDR_BYTES(&addr));

	// SET THE PORT TO PROMOCIOUS
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}
// END OF PORT INITIALIZATION

// OPEN FILE
static FILE *open_file(const char *filename)
{
	FILE *f = fopen(filename, "a+");
	if (f == NULL)
	{
		printf("Error opening file!\n");
		exit(1);
	}
	return f;
}

// PRINT OUT STATISTICS
static void
print_stats(void)
{
	unsigned int portid;

	const char clr[] = {27, '[', '2', 'J', '\0'};
	const char topLeft[] = {27, '[', '1', ';', '1', 'H', '\0'};

	// Clear screen and move to top left
	// printf("%s%s", clr, topLeft);
	// printf("POLICY SERVER\n");
	// printf("\nRefreshed every %d seconds. "
	// 	   "Send every %d minutes.\n",
	// 	   TIMER_PERIOD_STATS, TIMER_PERIOD_SEND);
	// printf("\nPort statistics ====================================");

	// for (portid = 0; portid < 2; portid++)
	// {
	// 	printf("\nStatistics for port %u ------------------------------"
	// 		   "\nPackets sent count: %18" PRIu64
	// 		   "\nPackets sent size: %19" PRIu64
	// 		   "\nPackets received count: %14" PRIu64
	// 		   "\nPackets received size: %15" PRIu64
	// 		   "\nPackets dropped: %21" PRIu64
	// 		   "\nRST to Client:  %22" PRIu64
	// 		   "\nRST to Server:          %14" PRIu64
	// 		   "\nThroughput: %26" PRIu64,
	// 		   portid,
	// 		   port_statistics[portid].tx_count,
	// 		   port_statistics[portid].tx_size,
	// 		   port_statistics[portid].rx_count,
	// 		   port_statistics[portid].rx_size,
	// 		   port_statistics[portid].dropped,
	// 		   port_statistics[portid].rstClient,
	// 		   port_statistics[portid].rstServer,
	// 		   port_statistics[portid].throughput);
	// }
	// printf("\n=====================================================");

	fflush(stdout);
}

// PRINT STATISTICS HEADER INTO CSV FILE
static void print_stats_csv_header(FILE *f)
{
	fprintf(f, "ps_id,rst_client,rst_server,rx_count,tx_count,rx_size,tx_size,time,throughput\n"); // Header row
}

// PRINT STATISTICS INTO CSV FILE
static void print_stats_csv(FILE *f, char *timestamp)
{
	// Write data to the CSV file
	fprintf(f, "%d,%ld,%ld,%ld,%ld,%ld,%ld,%s,%ld\n", 1, port_statistics[1].rstClient, port_statistics[1].rstServer, port_statistics[0].rx_count, port_statistics[1].tx_count, port_statistics[0].rx_size, port_statistics[1].tx_size, timestamp, port_statistics[1].throughput);
}

// CLEAR THE STATS STRUCT
static void clear_stats(void)
{
	rte_eth_stats_reset(0);
	rte_eth_stats_reset(1);
	memset(port_statistics, 0, RTE_MAX_ETHPORTS * sizeof(struct port_statistics_data));
}

// CONFIG FILE LOADER
int load_config_file()
{
	FILE *configFile = fopen("config/policyServer.cfg", "r");
	if (configFile == NULL)
	{
		printf("Error opening configuration file");
		return 1;
	}

	char line[256];
	char key[256];
	char value[256];

	while (fgets(line, sizeof(line), configFile))
	{
		if (sscanf(line, "%255[^=]= %255[^\n]", key, value) == 2)
		{
			if (strcmp(key, "MAX_PACKET_LEN") == 0)
			{
				MAX_PACKET_LEN = atoi(value);
				printf("MAX_PACKET_LEN: %d\n", MAX_PACKET_LEN);
			}
			else if (strcmp(key, "RX_RING_SIZE") == 0)
			{
				RX_RING_SIZE = atoi(value);
				printf("RX_RING_SIZE: %d\n", RX_RING_SIZE);
			}
			else if (strcmp(key, "TX_RING_SIZE") == 0)
			{
				TX_RING_SIZE = atoi(value);
				printf("TX_RING_SIZE: %d\n", TX_RING_SIZE);
			}
			else if (strcmp(key, "NUM_MBUFS") == 0)
			{
				NUM_MBUFS = atoi(value);
				printf("NUM_MBUFS: %d\n", NUM_MBUFS);
			}
			else if (strcmp(key, "MBUF_CACHE_SIZE") == 0)
			{
				MBUF_CACHE_SIZE = atoi(value);
				printf("MBUF_CACHE_SIZE: %d\n", MBUF_CACHE_SIZE);
			}
			else if (strcmp(key, "BURST_SIZE") == 0)
			{
				BURST_SIZE = atoi(value);
				printf("BURST_SIZE: %d\n", BURST_SIZE);
			}
			else if (strcmp(key, "MAX_TCP_PAYLOAD_LEN") == 0)
			{
				MAX_TCP_PAYLOAD_LEN = atoi(value);
				printf("MAX_TCP_PAYLOAD_LEN: %d\n", MAX_TCP_PAYLOAD_LEN);
			}
			else if (strcmp(key, "STAT_FILE") == 0)
			{
				strcpy(STAT_FILE, value);
				printf("STAT_FILE: %s\n", STAT_FILE);
			}
			else if (strcmp(key, "STAT_FILE_EXT") == 0)
			{
				strcpy(STAT_FILE_EXT, value);
				printf("STAT_FILE_EXT: %s\n", STAT_FILE_EXT);
			}
			else if (strcmp(key, "TIMER_PERIOD") == 0)
			{
				TIMER_PERIOD = atoi(value);
				printf("TIMER_PERIOD: %d\n", TIMER_PERIOD);
			}
			else if (strcmp(key, "TIMER_PERIOD_STATS") == 0)
			{
				TIMER_PERIOD_STATS = atoi(value);
				printf("TIMER_PERIOD_STATS: %d\n", TIMER_PERIOD_STATS);
			}
			else if (strcmp(key, "TIMER_PERIOD_SEND") == 0)
			{
				TIMER_PERIOD_SEND = atoi(value);
				printf("TIMER_PERIOD_SEND: %d\n", TIMER_PERIOD_SEND);
			}
		}
	}

	fclose(configFile);
	return 0;
}

// TERMINATION SIGNAL HANDLER
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
// END OF TERMINATION SIGNAL HANDLER

// PRINT STATISTICS PROCESS
static void print_stats_file(int *last_run_stat, int *last_run_file, FILE **f_stat)
{
	int current_sec;
	char time_str[80];
	char time_str_file[80];
	const char *format = "%Y-%m-%dT%H:%M:%S";
	struct tm *tm_info, *tm_rounded;
	time_t now, rounded;

	time(&now);
	tm_info = localtime(&now);
	current_sec = tm_info->tm_sec;
	if (current_sec % TIMER_PERIOD_STATS == 0 && current_sec != *last_run_stat)
	{
		char *filename = (char *)calloc(100, 100);

		// get the current minute
		int current_min = tm_info->tm_min;

		// check file
		if (!*f_stat)
		{
			// get the rounded time
			int remaining_seconds = current_min % TIMER_PERIOD_SEND * 60 + current_sec;
			rounded = now - remaining_seconds;
			tm_rounded = localtime(&rounded);

			// convert the time to string
			strftime(time_str_file, sizeof(time_str_file), format, tm_rounded);

			// create the filename
			strcat(filename, STAT_FILE);
			strcat(filename, time_str_file);
			strcat(filename, STAT_FILE_EXT);
			printf("first open file");
			*f_stat = open_file(filename);

			// print the header of the statistics file
			print_stats_csv_header(*f_stat);

			// free the string
			free(filename);
			*last_run_file = tm_rounded->tm_min;

			// Set the time to now
			tm_info = localtime(&now); // TODO: not efficient because already called before
		}

		// convert the time to string
		strftime(time_str, sizeof(time_str), format, tm_info);

		// print out the stats to csv
		print_stats_csv(*f_stat, time_str);
		fflush(*f_stat);

		// clear the stats
		clear_stats();

		if (current_min % TIMER_PERIOD_SEND == 0 && current_min != *last_run_file)
		{
			// create the filename
			strcat(filename, STAT_FILE);
			strcat(filename, time_str);
			strcat(filename, STAT_FILE_EXT);
			printf("open file");
			*f_stat = open_file(filename);

			// print the header of the statistics file
			print_stats_csv_header(*f_stat);

			// free the string
			free(filename);

			// set the last run file
			*last_run_file = current_min;
		}

		// Set the last run time
		*last_run_stat = current_sec;
	}
}
static inline char *extractDomainfromHTTPS(struct rte_mbuf *pkt)
{
	// Extract Ethernet header
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		printf("Packet is not an IPv4 packet\n");
		return NULL;
	}

	// Extract IPv4 header
	struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

	if (ip_hdr->next_proto_id != IPPROTO_TCP)
	{
		printf("Packet is not a TCP packet\n");
		return NULL;
	}

	// Extract TCP header
	struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + sizeof(struct rte_ipv4_hdr));

	// Calculate the offset to the TLS header (if TLS is in use)
	int tls_offset = (tcp_hdr->data_off & 0xf0) >> 2;

	if (tls_offset <= 0)
	{
		printf("No TLS header found in the packet\n");
		return NULL;
	}

	// Calculate the total length of the TLS payload
	int tls_payload_length = ntohs(ip_hdr->total_length) - (sizeof(struct rte_ipv4_hdr) + (tcp_hdr->data_off >> 4) * 4);

	if (tls_payload_length <= 0)
	{
		printf("No TLS payload found in the packet\n");
		return NULL;
	}

	int start_offset = 76;
	int end_offset = 77;

	if (start_offset < 0 || end_offset >= tls_payload_length)
	{
		printf("Invalid byte range specified for the TLS payload\n");
		return NULL;
	}

	// Extract the TLS payload as a pointer to uint8_t
	uint8_t *tls_payload = (uint8_t *)tcp_hdr + tls_offset;
	uint16_t combinedValue = (uint16_t)tls_payload[start_offset] << 8 | (uint16_t)tls_payload[end_offset];

	// Process the specific range of bytes in the TLS payload
	int counter = 82 + combinedValue;
	char extractedName[256]; // Assuming a maximum name length of 256 characters
	int nameIndex = 0;		 // Index for the extractedName array

	while (1)
	{
		uint16_t type = (uint16_t)tls_payload[counter] << 8 | (uint16_t)tls_payload[counter + 1];

		if (type == 0)
		{
			uint16_t namelength = (uint16_t)tls_payload[counter + 7] << 8 | (uint16_t)tls_payload[counter + 8];

			for (int i = 0; i < namelength; i++)
			{
				extractedName[nameIndex] = (char)tls_payload[counter + 9 + i];
				nameIndex++;
			}
			extractedName[nameIndex] = '\0'; // Null-terminate the string
			// printf("Name Length: %d\n", namelength);
			// printf("Extracted Name: %s\n", extractedName);

			// Dynamically allocate memory for the string to return
			char *result = (char *)malloc(strlen(extractedName) + 1);
			strcpy(result, extractedName);
			return result;
		}
		else
		{
			uint16_t length = (uint16_t)tls_payload[counter + 2] << 8 | (uint16_t)tls_payload[counter + 3];
			counter += length + 4;
		}
	}
}

static inline char *extractDomainfromHTTP(struct rte_mbuf *pkt)
{
	printf("From HTTP Extract");
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		printf("Not an IPv4 Packet\n");
		return NULL;
	}

	struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

	if (ipv4_hdr->next_proto_id != IPPROTO_TCP)
	{
		printf("Not a TCP Packet\n");
		return NULL;
	}

	struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));

	// Calculate the offset to the HTTP payload
	int payload_offset = ((tcp_hdr->data_off & 0xf0) >> 2);

	if (payload_offset <= 0)
	{
		printf("No HTTP payload found in the packet\n");
		return NULL;
	}

	// Pointer to the HTTP payload
	char *payload = (char *)tcp_hdr + payload_offset;

	char *host_start = strstr(payload, "Host:");
	if (host_start != NULL)
	{
		char *host_end = strchr(host_start, '\r');
		if (host_end != NULL)
		{
			// Extract the HTTP host.
			char *host = (char *)malloc(256 * sizeof(char)); // Assuming a reasonable max size for the host.
			if (host == NULL)
			{
				printf("Memory allocation failed\n");
				return NULL;
			}
			int host_length = host_end - host_start - 6; // Subtract "Host: "
			if (host_length > 0 && host_length < 256)
			{
				strncpy(host, host_start + 6, host_length);
				host[host_length] = '\0'; // Null-terminate the string
				printf("HTTP Host: %s\n", host);
				return host;
			}
			free(host); // Free allocated memory in case of errors
		}
	}

	return NULL; // Return NULL if the HTTP host is not found or an error occurs.
}

static inline void reset_tcp_client(struct rte_mbuf *rx_pkt)
{
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

	// Check if it's an IPv4 packet
	if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
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
	}
}

static inline void reset_tcp_server(struct rte_mbuf *rx_pkt)
{

	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		tcp_hdr->sent_seq = rte_cpu_to_be_32(tcp_hdr->sent_seq + 1);
		tcp_hdr->recv_ack = rte_cpu_to_be_32(tcp_hdr->sent_seq);
		tcp_hdr->tcp_flags = RTE_TCP_RST;
		// Calculate TCP payload length

		ip_hdr->total_length = rte_cpu_to_be_16(40);

		// // Calculate and set the new IP and TCP checksums (optional)
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
			sqlite3_exec(db, "CREATE TABLE policies (ip_address TEXT, domain TEXT)", NULL, 0, NULL);
		}
	}
}

static inline bool ip_checker(struct rte_mbuf *rx_pkt)
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
	// printf("Dest IP: %s\n", dest_ip_str);
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
	// printf("Count: %d\n", count)
	return count > 0;
}

static inline bool domain_checker(char *domain)
{
	if (domain == NULL)
	{
		return false;
	}

	if (!db)
	{
		// Database is not initialized
		return false;
	}

	// Prepare an SQL query to check if the destination IP exists in the database
	char query[256];
	snprintf(query, sizeof(query), "SELECT COUNT(*) FROM policies WHERE domain = '%s'", domain);

	// Execute the SQL query
	sqlite3_stmt *stmt;
	int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

	if (result != SQLITE_OK)
	{
		// Handle query preparation error
		printf("Error preparing SQL query: %s\n", sqlite3_errmsg(db));
		return false;
	}

	// Execute the query and check if the domain exists in the database
	int count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		count = sqlite3_column_int(stmt, 0);
	}

	// Finalize the statement
	sqlite3_finalize(stmt);
	printf("Count: %d\n", count);
	return count > 0;
}

// ======================================================= THE LCORE FUNCTION =======================================================
static inline void
lcore_main_process(void)
{
	// initialization
	char *extractedName;
	uint16_t port;
	init_database();
	uint64_t timer_tsc = 0;

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
			   "not be optimal.\n",
			   port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
		   rte_lcore_id());

	// Main work of application loop
	while (!force_quit)
	{

		/* Get a burst of RX packets from the first port of the pair. */
		struct rte_mbuf *rx_bufs[BURST_SIZE];
		const uint16_t rx_count = rte_eth_rx_burst(0, 0, rx_bufs, BURST_SIZE);
		uint64_t start_tsc = rte_rdtsc();
		for (uint16_t i = 0; i < rx_count; i++)
		{
			struct rte_mbuf *rx_pkt = rx_bufs[i];
			// extractDomainfromHTTP(rx_pkt);
			// extractedName = extractDomainfromHTTPS(rx_pkt);
			// printf("Extracted Name333333: %s\n", extractedName);

			if (domain_checker(extractDomainfromHTTP(rx_pkt)))
			{

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
					rte_pktmbuf_free(rst_pkt_client); // Free the modified packet
				}
				else
				{
					port_statistics[1].rstClient++;
				}

				const uint16_t rst_server_tx_count = rte_eth_tx_burst(1, 0, &rst_pkt_server, 1);
				if (rst_server_tx_count == 0)
				{
					printf("Error sending packet to server\n");
					rte_pktmbuf_free(rst_pkt_server); // Free the modified packet
				}
				else
				{
					port_statistics[1].rstServer++;
				}
			}
			uint64_t end_tsc = rte_rdtsc(); // Get end timestamp
			uint64_t processing_time = end_tsc - start_tsc;
			printf("Processing time: %" PRIu64 " cycles\n", processing_time);
			rte_pktmbuf_free(rx_pkt); // Free the original packet
		}
	}
}

static inline void
lcore_stats_process(void)
{
	// Variable declaration
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc; // for timestamp
	const uint64_t drain_tsc = rte_get_tsc_hz() / 1000;
	int last_run_stat = 0;
	int last_run_file = 0;
	uint64_t start_tx_size_0 = 0, end_tx_size_0 = 0;
	uint64_t start_rx_size_1 = 0, end_rx_size_1 = 0;
	double throughput_0 = 0.0, throughput_1 = 0.0;
	FILE *f_stat = NULL;

	timer_tsc = 0;
	prev_tsc = 0;

	while (!force_quit)
	{
		// Get the current timestamp
		cur_tsc = rte_rdtsc();

		// Get the difference between the current timestamp and the previous timestamp
		diff_tsc = cur_tsc - prev_tsc;

		if (unlikely(diff_tsc > drain_tsc))
		{
			if (TIMER_PERIOD > 0)
			{

				timer_tsc += 1;

				// Check if the difference is greater than the timer period
				if (unlikely(timer_tsc >= TIMER_PERIOD))
				{
					printf("timer_tsc: %ld\n", timer_tsc);

					// Get the statistics
					rte_eth_stats_get(1, &stats_1);
					rte_eth_stats_get(0, &stats_0);

					// Update the statistics
					port_statistics[1].rx_count = stats_1.ipackets;
					port_statistics[1].tx_count = stats_1.opackets;
					port_statistics[1].rx_size = stats_1.ibytes;
					port_statistics[1].tx_size = stats_1.obytes;
					port_statistics[1].dropped = stats_1.imissed;
					port_statistics[0].rx_count = stats_0.ipackets;
					port_statistics[0].tx_count = stats_0.opackets;
					port_statistics[0].rx_size = stats_0.ibytes;
					port_statistics[0].tx_size = stats_0.obytes;
					port_statistics[0].dropped = stats_0.imissed;

					// Calculate the throughput

					port_statistics[0].throughput = port_statistics[0].rx_size / TIMER_PERIOD_STATS;
					port_statistics[1].throughput = port_statistics[1].tx_size / TIMER_PERIOD_STATS;

					// Update the start_tx_size for the next period
					start_tx_size_0 = end_tx_size_0;
					start_rx_size_1 = end_rx_size_1;

					// Print the statistics
					print_stats();

					// Print Statistcs to file
					print_stats_file(&last_run_stat, &last_run_file, &f_stat);

					// Reset the timer
					timer_tsc = 0;
				}
			}
			// Reset the previous timestamp
			prev_tsc = cur_tsc;
		}
	}
}

// ======================================================= THE MAIN FUNCTION =======================================================
int main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;
	unsigned lcore_id, lcore_main = 0, lcore_stats = 0;

	// load the config file
	if (load_config_file())
	{
		rte_exit(EXIT_FAILURE, "Cannot load the config file\n");
	}

	// Initializion the Environment Abstraction Layer (EAL)
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	// force quit handler
	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// clean the data
	memset(port_statistics, 0, 32 * sizeof(struct port_statistics_data));

	// count the number of ports to send and receive
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	// allocates the mempool to hold the mbufs
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
										MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	// check the mempool allocation
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	// initializing ports
	RTE_ETH_FOREACH_DEV(portid)
	if (port_init(portid, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n",
				 portid);

	// count the number of lcore
	if (rte_lcore_count() < 2)
		rte_exit(EXIT_FAILURE, "lcore must be more than equal 2\n");

	printf("assign lcore \n");

	RTE_LCORE_FOREACH_WORKER(lcore_id)
	{
		printf("lcore id : %u\n", lcore_id);
		printf("lcore main : %u\n", lcore_main == 0);
		printf("lcore stats : %u\n", lcore_stats);
		if (lcore_id == (unsigned int)lcore_main ||
			lcore_id == (unsigned int)lcore_stats)
		{
			printf("continue \n");
			continue;
		}
		if (lcore_main == 0)
		{
			lcore_main = lcore_id;
			printf("main on core %u\n", lcore_id);
			continue;
		}
		if (lcore_stats == 0)
		{
			lcore_stats = lcore_id;
			printf("Stats on core %u\n", lcore_id);
			continue;
		}
	}

	// run the lcore main function
	rte_eal_remote_launch((lcore_function_t *)lcore_main_process,
						  NULL, lcore_main);

	// run the stats
	rte_eal_remote_launch((lcore_function_t *)lcore_stats_process,
						  NULL, lcore_stats);

	// wait all lcore stopped
	RTE_LCORE_FOREACH_WORKER(lcore_id)
	{
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}
	// clean up the EAL
	rte_eal_cleanup();
}
