// ======================================================= THE LIBRARY =======================================================

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
#include <unistd.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <librdkafka/rdkafka.h>

// DPDK library
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_pdump.h>

#define CACHE_SIZE 10000
#define RTE_TCP_RST 0x04
#define MAX_STRINGS 64
#define KAFKA_TOPIC "dpdk-blocked-list"
#define KAFKA_BROKER "192.168.0.90:9092"

typedef enum
{
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_ERROR
} LogLevel;

uint32_t MAX_PACKET_LEN;

uint32_t RX_RING_SIZE;
uint32_t TX_RING_SIZE;
uint32_t NUM_MBUFS;
uint32_t MBUF_CACHE_SIZE;
uint32_t BURST_SIZE;
uint32_t MAX_TCP_PAYLOAD_LEN;
static uint32_t TIMER_PERIOD_STATS;
static uint32_t TIMER_PERIOD_SEND;
static const char *db_path = "/home/ubuntu/policy.db";
char PS_ID[200];
char STAT_FILE[100];
char STAT_FILE_EXT[100];
char HOSTNAME[100];
char hitCount[CACHE_SIZE][MAX_STRINGS];
uint64_t hitCounter = 0;
static sqlite3 *db;
clock_t start, end;
double service_time = 0, avg_service_time = 0;
int count_service_time = 0;
int countFlag = 0;
uint64_t rstServer_http = 0;
uint64_t rstClient_http = 0;
uint64_t rstServer_tls = 0;
uint64_t rstClient_tls = 0;
struct hit_counter
{
	char id[MAX_STRINGS];
	uint64_t hit_count;
};

struct port_statistics_data
{
	uint64_t tx_count;
	uint64_t rx_count;
	uint64_t tx_size;
	uint64_t rx_size;
	uint64_t dropped;
	uint64_t rstClient;
	uint64_t rstServer;
	long int throughput;
	uint64_t err_rx;
	uint64_t err_tx;
	uint64_t mbuf_err;
} __rte_cache_aligned;
struct port_statistics_data port_statistics[RTE_MAX_ETHPORTS];
struct rte_eth_stats stats_0;
struct rte_eth_stats stats_1;
struct rte_eth_stats stats_2;
struct rte_eth_stats stats_3;
static volatile bool force_quit;
static struct hit_counter db_hit_count[CACHE_SIZE];

typedef struct
{
	char domain[MAX_STRINGS];
	char id[MAX_STRINGS];
} DomainCache;

static DomainCache domain_cache[CACHE_SIZE];
static int domainCacheSize = 0;

typedef struct
{
	char ip_address[INET_ADDRSTRLEN];
	char id[MAX_STRINGS];
} IPCache;

static IPCache ip_cache[CACHE_SIZE];
static int ipCacheSize = 0;


/**
 * Returns the string representation of the given log level.
 *
 * @param level The log level to get the string representation for.
 * @return The string representation of the log level.
 */
const char *getLogLevelString(LogLevel level)
{
	switch (level)
	{
	case LOG_LEVEL_INFO:
		return "INFO";
	case LOG_LEVEL_WARNING:
		return "WARNING";
	case LOG_LEVEL_ERROR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

/**
 * Logs a message to a log file with the specified log level, filename, line number, and format.
 *
 * @param level The log level of the message.
 * @param filename The name of the file where the log message is called.
 * @param line The line number where the log message is called.
 * @param format The format string for the log message.
 * @param ... Additional arguments to be formatted according to the format string.
 *
 * @return void
 */
void logMessage(LogLevel level, const char *filename, int line, const char *format, ...)
{
	// Open the log file in append mode
	FILE *file = fopen("logs/log.txt", "a");
	if (file == NULL)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error opening file %s\n", filename);
		return;
	}

	// Get the current time
	time_t rawtime;
	struct tm *timeinfo;
	char timestamp[20];
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

	// Write the timestamp and log level to the file
	fprintf(file, "[%s] [%s] [%s:%d] - ", timestamp, getLogLevelString(level), filename, line);

	// Write the formatted message to the file
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);

	// Close the file
	fclose(file);
}

/**
 * Consumes a Kafka message and updates an SQLite database based on the message content.
 *
 * @param rkmessage A pointer to the Kafka message to consume.
 * @param db A pointer to the SQLite database.
 */
void msg_consume(rd_kafka_message_t *rkmessage, sqlite3 *db)
{
	if (rkmessage->err)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Kafka error: %s\n", rd_kafka_message_errstr(rkmessage));
		return;
	}

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Received message: %.*s\n", (int)rkmessage->len, (char *)rkmessage->payload);

	// Parse JSON message
	json_error_t error;
	json_t *root = json_loadb(rkmessage->payload, rkmessage->len, 0, &error);
	if (!root)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "JSON parsing error: %s\n", error.text);
		return;
	}

	const char *type_str = NULL;
	const char *createdBlockedListKey = NULL;

	// Check 'type' field
	json_t *type = json_object_get(root, "type");
	if (!type || !json_is_string(type))
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Key 'type' not found or not a string\n");
		goto cleanup;
	}
	type_str = json_string_value(type);
	if (!type_str)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to get 'type' value as string\n");
		goto cleanup;
	}

	// Check 'createdBlockedList' based on 'type'
	if (strcmp(type_str, "create") == 0)
		createdBlockedListKey = "createdBlockedList";
	else if (strcmp(type_str, "update") == 0)
		createdBlockedListKey = "updatedBlockedList";
	else if (strcmp(type_str, "delete") == 0)
		createdBlockedListKey = "deletedBlockedList";
	else
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Unsupported 'type' value: %s\n", type_str);
		goto cleanup;
	}

	// Extract 'createdBlockedList'
	json_t *createdBlockedList = json_object_get(root, createdBlockedListKey);
	if (!createdBlockedList || !json_is_object(createdBlockedList))
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Key '%s' not found or not an object\n", createdBlockedListKey);
		goto cleanup;
	}

	// Extract 'domain', 'ip_add', and 'id' fields
	json_t *domain = json_object_get(createdBlockedList, "domain");
	json_t *ip_add = json_object_get(createdBlockedList, "ip_add");
	json_t *id = json_object_get(createdBlockedList, "id");

	if (!domain || !json_is_string(domain) || !ip_add || !json_is_string(ip_add) || !id || !json_is_string(id))
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Missing or invalid keys in 'createdBlockedList'\n");
		goto cleanup;
	}

	const char *domain_str = json_string_value(domain);
	const char *ip_str = json_string_value(ip_add);
	const char *id_str = json_string_value(id);

	if (!domain_str || !ip_str || !id_str)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to get values from 'createdBlockedList'\n");
		goto cleanup;
	}

	// Update SQLite database
	char sql_query[256];
	int query_len = 0;

	if (strcmp(type_str, "create") == 0)
		query_len = snprintf(sql_query, sizeof(sql_query), "INSERT INTO policies (id, domain, ip_address) VALUES ('%s', '%s', '%s');", id_str, domain_str, ip_str);
	else if (strcmp(type_str, "update") == 0)
	{
		query_len = snprintf(sql_query, sizeof(sql_query), "UPDATE policies SET domain='%s', ip_address='%s' WHERE id='%s';", domain_str, ip_str, id_str);
	}
	else if (strcmp(type_str, "delete") == 0)
	{
		query_len = snprintf(sql_query, sizeof(sql_query), "DELETE FROM policies WHERE id='%s';", id_str);
	}
	else
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Unsupported 'type' value: %s\n", type_str);
		goto cleanup;
	}

	if (query_len <= 0 || query_len >= sizeof(sql_query))
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "SQL query creation error\n");
		goto cleanup;
	}

	char *errmsg = NULL;
	int sqlite_result = sqlite3_exec(db, sql_query, NULL, 0, &errmsg);
	if (sqlite_result != SQLITE_OK)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
	else
	{
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "%s updated to the database\n", domain_str);
	}

cleanup:
	if (root)
		json_decref(root);
}




/**
 * This function configures and starts an Ethernet port with the specified port number.
 * It sets up the receive (Rx) and transmit (Tx) queues, allocates memory for the queues,
 * and enables promiscuous mode for the port.
 *
 * @param port The port number to initialize.
 * @param mbuf_pool The memory pool to use for allocating mbufs.
 * @return 0 on success, a negative value on error.
 */

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
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error during getting device (port %u) info: %s\n",
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

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			   port, RTE_ETHER_ADDR_BYTES(&addr));

	// SET THE PORT TO PROMOCIOUS
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

/**
 * Opens a file in append mode and returns a file pointer.
 *
 * @param filename The name of the file to be opened.
 * @return A file pointer to the opened file.
 * @throws An error message and exits the program if the file cannot be opened.
 */
static FILE *open_file(const char *filename)
{
	FILE *f = fopen(filename, "a+");
	if (f == NULL)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error opening file %s\n", filename);
		rte_exit(EXIT_FAILURE, "Error opening file %s\n", filename);
	}
	return f;
}


/**
 * This function resets the statistics data for all Ethernet ports by setting
 * the memory to zero.
 */
static void clear_stats(void)
{
	memset(port_statistics, 0, RTE_MAX_ETHPORTS * sizeof(struct port_statistics_data));
}

/**
 * This function prints the statistics for each port, including the number of packets sent and received,
 * packet sizes, dropped packets, TCP RST counts, throughput, and packet errors.
 * The statistics are refreshed periodically based on the TIMER_PERIOD_STATS value.
 *
 * @param last_run_print A pointer to the last time the statistics were printed.
 */
static void
print_stats(int *last_run_print)
{
	unsigned int portid;

	const char clr[] = {27, '[', '2', 'J', '\0'};
	const char topLeft[] = {27, '[', '1', ';', '1', 'H', '\0'};

	// set timer
	time_t rawtime;
	struct tm *timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);

	if (timeinfo->tm_sec % TIMER_PERIOD_STATS == 0 && timeinfo->tm_sec != *last_run_print)
	{
		// Clear screen and move to top left
		printf("%s%s", clr, topLeft);
		printf("POLICY SERVER\n");
		printf("\nRefreshed every %d seconds. "
			   "Send every %d minutes.\n",
			   TIMER_PERIOD_STATS, TIMER_PERIOD_SEND);
		printf("\nPort statistics ====================================");

		for (portid = 0; portid < 4; portid++)
		{
			printf("\nStatistics for port %u ------------------------------"
				   "\nPackets sent count: %18" PRIu64
				   "\nPackets sent size: %19" PRIu64
				   "\nPackets received count: %14" PRIu64
				   "\nPackets received size: %15" PRIu64
				   "\nPackets dropped: %21" PRIu64
				   "\nTCP RST to Client: %19" PRIu64
				   "\nTCP RST to Server: %19" PRIu64
				   "\nThroughput: %26" PRId64
				   "\nPacket errors rx: %20" PRIu64
				   "\nPacket errors tx: %20" PRIu64,
				   portid,
				   port_statistics[portid].tx_count,
				   port_statistics[portid].tx_size,
				   port_statistics[portid].rx_count,
				   port_statistics[portid].rx_size,
				   port_statistics[portid].dropped,
				   port_statistics[portid].rstClient,
				   port_statistics[portid].rstServer,
				   port_statistics[portid].throughput,
				   port_statistics[portid].err_rx,
				   port_statistics[portid].err_tx);
		}
		printf("\n=====================================================");
		fflush(stdout);
		clear_stats();
		*last_run_print = timeinfo->tm_sec;
	}
}

/**
 * This function writes the header row to the specified file in CSV format.
 * The header row contains the names of the different statistics fields.
 *
 * @param f The file pointer to write the header to.
 */
static void print_stats_csv_header(FILE *f)
{
	fprintf(f, "ps_id,rstClient_http,rstServer_http,rstClient_tls,rstServer_tls,rx_i_http_count,tx_i_http_count,rx_i_http_size,tx_i_http_size,rx_i_http_drop,rx_i_http_error,tx_i_http_error,rx_i_http_mbuf,rx_i_tls_count,tx_i_tls_count,rx_i_tls_size,tx_i_tls_size,rx_i_tls_drop,rx_i_tls_error,tx_i_tls_error,rx_i_tls_mbuf,rx_o_http_count,tx_o_http_count,rx_o_http_size,tx_o_http_size,rx_o_http_drop,rx_o_http_error,tx_o_http_error,rx_o_http_mbuf,rx_o_tls_count,tx_o_tls_count,rx_o_tls_size,tx_o_tls_size,rx_o_tls_drop,rx_o_tls_error,tx_o_tls_error,rx_o_tls_mbuf,time,throughput_i_http, throughput_i_tls, throughput_o_http,throughput_o_tls\n"); // Header row
}

/**
 * This function takes a file pointer and a timestamp as input and writes the statistics data to the CSV file.
 * The statistics data includes various metrics such as packet counts, packet sizes, errors, and throughput for different ports.
 *
 * @param f         The file pointer to the CSV file.
 * @param timestamp The timestamp to be included in the CSV file.
 */
static void print_stats_csv(FILE *f, char *timestamp)
{
	// Write data to the CSV file
	fprintf(f, "%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%s,%ld,%ld,%ld,%ld\n", PS_ID, port_statistics[2].rstClient, port_statistics[2].rstServer, port_statistics[3].rstClient, port_statistics[3].rstServer,
			port_statistics[0].rx_count, port_statistics[0].tx_count, port_statistics[0].rx_size, port_statistics[0].tx_size, port_statistics[0].dropped, port_statistics[0].err_rx, port_statistics[0].err_tx, port_statistics[0].mbuf_err,
			port_statistics[1].rx_count, port_statistics[1].tx_count, port_statistics[1].rx_size, port_statistics[1].tx_size, port_statistics[1].dropped, port_statistics[1].err_rx, port_statistics[1].err_tx, port_statistics[1].mbuf_err,
			port_statistics[2].rx_count, port_statistics[2].tx_count, port_statistics[2].rx_size, port_statistics[2].tx_size, port_statistics[2].dropped, port_statistics[2].err_rx, port_statistics[2].err_tx, port_statistics[2].mbuf_err,
			port_statistics[3].rx_count, port_statistics[3].tx_count, port_statistics[3].rx_size, port_statistics[3].tx_size, port_statistics[3].dropped, port_statistics[3].err_rx, port_statistics[3].err_tx, port_statistics[3].mbuf_err,
			timestamp, port_statistics[0].throughput, port_statistics[1].throughput, port_statistics[2].throughput, port_statistics[3].throughput);
}

/**
 * This function reads the configuration file located at "config/config.cfg" and sets the values of various variables based on the key-value pairs in the file.
 * The function expects the configuration file to be in the format "key = value", where the key is a string and the value is an integer or a string.
 */

int load_config_file()
{
	FILE *configFile = fopen("config/config.cfg", "r");
	if (configFile == NULL)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Cannot open the config file\n");
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
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "MAX_PACKET_LEN: %d\n", MAX_PACKET_LEN);
			}
			else if (strcmp(key, "RX_RING_SIZE") == 0)
			{
				RX_RING_SIZE = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "RX_RING_SIZE: %d\n", RX_RING_SIZE);
			}
			else if (strcmp(key, "TX_RING_SIZE") == 0)
			{
				TX_RING_SIZE = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "TX_RING_SIZE: %d\n", TX_RING_SIZE);
			}
			else if (strcmp(key, "NUM_MBUFS") == 0)
			{
				NUM_MBUFS = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "NUM_MBUFS: %d\n", NUM_MBUFS);
			}
			else if (strcmp(key, "MBUF_CACHE_SIZE") == 0)
			{
				MBUF_CACHE_SIZE = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "MBUF_CACHE_SIZE: %d\n", MBUF_CACHE_SIZE);
			}
			else if (strcmp(key, "BURST_SIZE") == 0)
			{
				BURST_SIZE = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "BURST_SIZE: %d\n", BURST_SIZE);
			}
			else if (strcmp(key, "MAX_TCP_PAYLOAD_LEN") == 0)
			{
				MAX_TCP_PAYLOAD_LEN = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "MAX_TCP_PAYLOAD_LEN: %d\n", MAX_TCP_PAYLOAD_LEN);
			}
			else if (strcmp(key, "STAT_FILE") == 0)
			{
				strcpy(STAT_FILE, value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "STAT_FILE: %s\n", STAT_FILE);
			}
			else if (strcmp(key, "STAT_FILE_EXT") == 0)
			{
				strcpy(STAT_FILE_EXT, value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "STAT_FILE_EXT: %s\n", STAT_FILE_EXT);
			}
			else if (strcmp(key, "TIMER_PERIOD_STATS") == 0)
			{
				TIMER_PERIOD_STATS = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "TIMER_PERIOD_STATS: %d\n", TIMER_PERIOD_STATS);
			}
			else if (strcmp(key, "TIMER_PERIOD_SEND") == 0)
			{
				TIMER_PERIOD_SEND = atoi(value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "TIMER_PERIOD_SEND: %d\n", TIMER_PERIOD_SEND);
			}
			else if (strcmp(key, "ID_PS") == 0)
			{
				strcpy(PS_ID, value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "PS ID: %s\n", PS_ID);
			}
			else if (strcmp(key, "HOSTNAME") == 0)
			{
				strcpy(HOSTNAME, value);
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "HOSTNAME: %s\n", HOSTNAME);
			}
		}
	}

	fclose(configFile);
	return 0;
}


/**
 * This function is responsible for handling the SIGINT and SIGTERM signals. When either of these signals is received,
 * the function logs a message indicating the signal received and sets the `force_quit` flag to true, indicating that
 * the program should prepare to exit.
 */
static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Signal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

/**
 * This function is used as a callback for the CURLOPT_WRITEFUNCTION option in a libcurl request.
 * It is called by libcurl whenever response data is received from the server.
 *
 * @param contents A pointer to the response data received from the server.
 * @param size The size of each element in the response data.
 * @param nmemb The number of elements in the response data.
 * @param userp A pointer to user-defined data passed to the CURLOPT_WRITEDATA option.
 *
 * @return The total number of bytes written.
 */
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t real_size = size * nmemb;
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Response: %.*s \n", (int)real_size, (char *)contents);
	return real_size;
}

/**
 * This function iterates over the `db_hit_count` array and creates a JSON object for each entry
 * with a non-zero hit count. The JSON object contains the entry's ID and hit count. The JSON
 * objects are then appended to the provided JSON array.
 *
 * @param jsonArray A pointer to the JSON array to populate.
 */
static void populate_json_hitcount(json_t *jsonArray)
{
	for (int i = 0; i < CACHE_SIZE; i++)
	{
		// Only create and append objects if hit_count > 0
		if (db_hit_count[i].hit_count > 0)
		{
			// Create object for each entry with non-zero hit_count
			json_t *jsonObject = json_object();
			json_object_set(jsonObject, "id", json_string(db_hit_count[i].id));
			json_object_set(jsonObject, "hit_count", json_integer(db_hit_count[i].hit_count));
			// Append the JSON object to the JSON array
			json_array_append(jsonArray, jsonObject);
		}
	}
	memset(db_hit_count, 0, sizeof(db_hit_count));
}
/**
 * This function takes a JSON array and a timestamp as input and populates a JSON object with various statistics data.
 * The statistics data includes counts, sizes, errors, drops, and throughput for different types of HTTP and TLS traffic.
 * The populated JSON object is then appended to the JSON array.
 *
 * @param jsonArray A pointer to the JSON array to which the populated JSON object will be appended.
 * @param timestamp A pointer to the timestamp string to be included in the JSON object.
 */

static void
populate_json_stats(json_t *jsonArray, char *timestamp)
{
	// Create object for the statistics
	json_t *jsonObject = json_object();

	// Populate the JSON object
	json_object_set(jsonObject, "ps_id", json_string(PS_ID));

	json_object_set(jsonObject, "rx_i_http_count", json_integer(port_statistics[0].rx_count));
	json_object_set(jsonObject, "tx_i_http_count", json_integer(port_statistics[0].tx_count));
	json_object_set(jsonObject, "rx_i_http_size", json_integer(port_statistics[0].rx_size));
	json_object_set(jsonObject, "tx_i_http_size", json_integer(port_statistics[0].tx_size));
	json_object_set(jsonObject, "rx_i_http_drop", json_integer(port_statistics[0].dropped));
	json_object_set(jsonObject, "rx_i_http_error", json_integer(port_statistics[0].err_rx));
	json_object_set(jsonObject, "tx_i_http_error", json_integer(port_statistics[0].err_tx));
	json_object_set(jsonObject, "rx_i_http_mbuf", json_integer(port_statistics[0].mbuf_err));

	json_object_set(jsonObject, "rx_i_tls_count", json_integer(port_statistics[1].rx_count));
	json_object_set(jsonObject, "tx_i_tls_count", json_integer(port_statistics[1].tx_count));
	json_object_set(jsonObject, "rx_i_tls_size", json_integer(port_statistics[1].rx_size));
	json_object_set(jsonObject, "tx_i_tls_size", json_integer(port_statistics[1].tx_size));
	json_object_set(jsonObject, "rx_i_tls_drop", json_integer(port_statistics[1].dropped));
	json_object_set(jsonObject, "rx_i_tls_error", json_integer(port_statistics[1].err_rx));
	json_object_set(jsonObject, "tx_i_tls_error", json_integer(port_statistics[1].err_tx));
	json_object_set(jsonObject, "rx_i_tls_mbuf", json_integer(port_statistics[1].mbuf_err));

	json_object_set(jsonObject, "rstClient_http", json_integer(port_statistics[2].rstClient));
	json_object_set(jsonObject, "rstServer_http", json_integer(port_statistics[2].rstServer));
	json_object_set(jsonObject, "rx_o_http_count", json_integer(port_statistics[2].rx_count));
	json_object_set(jsonObject, "tx_o_http_count", json_integer(port_statistics[2].tx_count));
	json_object_set(jsonObject, "rx_o_http_size", json_integer(port_statistics[2].rx_size));
	json_object_set(jsonObject, "tx_o_http_size", json_integer(port_statistics[2].tx_size));
	json_object_set(jsonObject, "rx_o_http_drop", json_integer(port_statistics[2].dropped));
	json_object_set(jsonObject, "rx_o_http_error", json_integer(port_statistics[2].err_rx));
	json_object_set(jsonObject, "tx_o_http_error", json_integer(port_statistics[2].err_tx));
	json_object_set(jsonObject, "rx_o_http_mbuf", json_integer(port_statistics[2].mbuf_err));

	json_object_set(jsonObject, "rstClient_tls", json_integer(port_statistics[3].rstClient));
	json_object_set(jsonObject, "rstServer_tls", json_integer(port_statistics[3].rstServer));
	json_object_set(jsonObject, "rx_o_tls_count", json_integer(port_statistics[3].rx_count));
	json_object_set(jsonObject, "tx_o_tls_count", json_integer(port_statistics[3].tx_count));
	json_object_set(jsonObject, "rx_o_tls_size", json_integer(port_statistics[3].rx_size));
	json_object_set(jsonObject, "tx_o_tls_size", json_integer(port_statistics[3].tx_size));
	json_object_set(jsonObject, "rx_o_tls_drop", json_integer(port_statistics[3].dropped));
	json_object_set(jsonObject, "rx_o_tls_error", json_integer(port_statistics[3].err_rx));
	json_object_set(jsonObject, "tx_o_tls_error", json_integer(port_statistics[3].err_tx));
	json_object_set(jsonObject, "rx_o_tls_mbuf", json_integer(port_statistics[3].mbuf_err));

	json_object_set(jsonObject, "time", json_string(timestamp));
	json_object_set(jsonObject, "throughput_i_http", json_integer(port_statistics[0].throughput));
	json_object_set(jsonObject, "throughput_i_tls", json_integer(port_statistics[1].throughput));
	json_object_set(jsonObject, "throughput_o_http", json_integer(port_statistics[2].throughput));
	json_object_set(jsonObject, "throughput_o_tls", json_integer(port_statistics[3].throughput));

	// Append the JSON object to the JSON array
	json_array_append(jsonArray, jsonObject);
}
/**
 * This function retrieves statistics for each port using the `rte_eth_stats_get()` function.
 * It then updates the statistics in the `port_statistics` array based on the retrieved values.
 * After updating the statistics, it clears the statistics for each port using the `rte_eth_stats_reset()` function.
 * Finally, it calculates the throughput for each port by dividing the received or transmitted size by the timer period.
 */
static void
collect_stats()
{
	// Get the statistics
	rte_eth_stats_get(1, &stats_1);
	rte_eth_stats_get(0, &stats_0);
	rte_eth_stats_get(2, &stats_2);
	rte_eth_stats_get(3, &stats_3);

	// Update the statistics
	port_statistics[3].rx_count = stats_3.ipackets;
	port_statistics[3].tx_count = stats_3.opackets;
	port_statistics[3].rx_size = stats_3.ibytes;
	port_statistics[3].tx_size = stats_3.obytes;
	port_statistics[3].dropped = stats_3.imissed;
	port_statistics[3].err_rx = stats_3.ierrors;
	port_statistics[3].err_tx = stats_3.oerrors;
	port_statistics[3].mbuf_err = stats_3.rx_nombuf;
	port_statistics[3].rstClient = rstClient_tls;
	port_statistics[3].rstServer = rstServer_tls;

	port_statistics[2].rx_count = stats_2.ipackets;
	port_statistics[2].tx_count = stats_2.opackets;
	port_statistics[2].rx_size = stats_2.ibytes;
	port_statistics[2].tx_size = stats_2.obytes;
	port_statistics[2].dropped = stats_2.imissed;
	port_statistics[2].err_rx = stats_2.ierrors;
	port_statistics[2].err_tx = stats_2.oerrors;
	port_statistics[2].mbuf_err = stats_2.rx_nombuf;
	port_statistics[2].rstClient = rstClient_http;
	port_statistics[2].rstServer = rstServer_http;

	port_statistics[0].rx_count = stats_0.ipackets;
	port_statistics[0].tx_count = stats_0.opackets;
	port_statistics[0].rx_size = stats_0.ibytes;
	port_statistics[0].tx_size = stats_0.obytes;
	port_statistics[0].dropped = stats_0.imissed;
	port_statistics[0].err_rx = stats_0.ierrors;
	port_statistics[0].err_tx = stats_0.oerrors;
	port_statistics[0].mbuf_err = stats_0.rx_nombuf;

	port_statistics[1].rx_count = stats_1.ipackets;
	port_statistics[1].tx_count = stats_1.opackets;
	port_statistics[1].rx_size = stats_1.ibytes;
	port_statistics[1].tx_size = stats_1.obytes;
	port_statistics[1].dropped = stats_1.imissed;
	port_statistics[1].err_rx = stats_1.ierrors;
	port_statistics[1].err_tx = stats_1.oerrors;
	port_statistics[1].mbuf_err = stats_1.rx_nombuf;

	// Clear the statistics
	rte_eth_stats_reset(0);
	rte_eth_stats_reset(1);
	rte_eth_stats_reset(2);
	rte_eth_stats_reset(3);
	rstClient_http = 0;
	rstServer_http = 0;
	rstClient_tls = 0;
	rstServer_tls = 0;

	// Calculate the throughput
	port_statistics[1].throughput = port_statistics[1].rx_size / TIMER_PERIOD_STATS;
	port_statistics[0].throughput = port_statistics[0].rx_size / TIMER_PERIOD_STATS;
	port_statistics[2].throughput = port_statistics[2].tx_size / TIMER_PERIOD_STATS;
	port_statistics[3].throughput = port_statistics[3].tx_size / TIMER_PERIOD_STATS;
}
/**
 * This function prints statistics to a file at regular intervals and also populates a JSON array with the statistics.
 * The statistics are printed in CSV format with a timestamp indicating the current time.
 * The file name for the statistics file is generated based on the current time.
 * @param last_run_stat A pointer to the last run time in seconds.
 * @param last_run_file A pointer to the last run time in minutes.
 * @param f_stat A pointer to the statistics file.
 * @param jsonArray A pointer to the JSON array to populate with the statistics.
 */
static void print_stats_file(int *last_run_stat, int *last_run_file, FILE **f_stat, json_t *jsonArray)
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
			*f_stat = open_file(filename);

			// print the header of the statistics file
			print_stats_csv_header(*f_stat);

			// free the string
			free(filename);
			*last_run_file = tm_rounded->tm_min;

			// Set the time to now
			tm_info = localtime(&now); // TODO: not efficient because already called before
		}

		collect_stats();

		// convert the time to string
		strftime(time_str, sizeof(time_str), format, tm_info);

		// print out the stats to csv
		print_stats_csv(*f_stat, time_str);
		populate_json_stats(jsonArray, time_str);
		fflush(*f_stat);

		// clear the stats

		if (current_min % TIMER_PERIOD_SEND == 0 && current_min != *last_run_file)
		{
			// create the filename
			strcat(filename, STAT_FILE);
			strcat(filename, time_str);
			strcat(filename, STAT_FILE_EXT);
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
/**
 * Sends the hit count data to the server.
 *
 * @param jsonArray A pointer to the JSON array containing the hit count data.
*/
static void send_hitcount_to_server(json_t *jsonArray)
{
	if (countFlag == 1)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Count Exceeds Threshold, Failed to Send\n");
		if (jsonArray)
			json_array_clear(jsonArray);
		countFlag = 0;
		return;
	}
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	char *jsonString = json_dumps(jsonArray, 0);
	char url[256];

	sprintf(url, "%s/ps/blocked-list-count", HOSTNAME);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();

	if (!curl)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to initialize CURL\n");
		goto cleanup;
	}

	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!headers)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to create headers\n");
		goto cleanup;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonString);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK)
	{
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Send Count failed: %s\n", curl_easy_strerror(res));
		goto cleanup;
	}
	else
	{
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Send Count Success: %s\n", jsonString);
	}

cleanup:
	if (headers)
		curl_slist_free_all(headers);

	if (curl)
		curl_easy_cleanup(curl);

	if (jsonString)
		free(jsonString);

	if (jsonArray)
		json_array_clear(jsonArray);

	curl_global_cleanup();
}

/**
 * Sends statistics to the server.
 *
 * @param jsonArray A pointer to the JSON array containing the statistics to be sent.
 */
static void
send_stats_to_server(json_t *jsonArray)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = curl_slist_append(headers, "Content-Type: application/json");

	char *jsonString = json_dumps(jsonArray, 0);
	char url[256];

	sprintf(url, "%s/ps/ps-packet", HOSTNAME);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();

	if (curl)
	{
		headers = curl_slist_append(headers, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonString);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
			logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Send Stats failed: %s\n", curl_easy_strerror(res));
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(jsonString);
		json_array_clear(jsonArray);
	}

	curl_global_cleanup();
}

/**
 * This function checks the current time and sends the statistics to the server
 * if the current minute is divisible by `TIMER_PERIOD_SEND` and is different
 * from the last time the statistics were sent.
 *
 * @param jsonArray A pointer to a JSON array containing the statistics data.
 * @param last_run_send A pointer to an integer representing the last minute
 *                      the statistics were sent.
 */
static void
send_stats(json_t *jsonArray, int *last_run_send)
{

	// Get the current time
	time_t rawtime;
	struct tm *timeinfo;
	char timestamp[20];
	time(&rawtime);
	timeinfo = localtime(&rawtime);

	int current_min = timeinfo->tm_min;
	if (current_min % TIMER_PERIOD_SEND == 0 && current_min != *last_run_send)
	{
		// send the statistics to the server
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Sending statistics to server\n");
		send_stats_to_server(jsonArray);
		*last_run_send = current_min;
	}
}

/**
 * This function extracts the domain name from an HTTPS packet.
 *
 * @param pkt A pointer to the packet from which to extract the domain name.
 * @return A pointer to the extracted domain name.
 */
static inline char *extractDomainfromHTTPS(struct rte_mbuf *pkt)
{
	// Extract Ethernet header
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		return NULL;
	}

	// Extract IPv4 header
	struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

	if (ip_hdr->next_proto_id != IPPROTO_TCP)
	{
		return NULL;
	}

	// Extract TCP header
	struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + sizeof(struct rte_ipv4_hdr));

	// Calculate the offset to the TLS header (if TLS is in use)
	int tls_offset = (tcp_hdr->data_off & 0xf0) >> 2;

	if (tls_offset <= 0)
	{
		return NULL;
	}

	// Calculate the total length of the TLS payload
	int tls_payload_length = ntohs(ip_hdr->total_length) - (sizeof(struct rte_ipv4_hdr) + (tcp_hdr->data_off >> 4) * 4);

	if (tls_payload_length <= 0)
	{
		return NULL;
	}

	int start_offset = 76;
	int end_offset = 77;

	if (start_offset < 0 || end_offset >= tls_payload_length)
	{
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
			extractedName[nameIndex] = '\0';

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

/**
 * This function extracts the domain name from an HTTP packet.
 *
 * @param pkt A pointer to the packet from which to extract the domain name.
 * @return A pointer to the extracted domain name.
 */
static inline char *extractDomainfromHTTP(struct rte_mbuf *pkt)
{
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		return NULL;
	}

	struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

	if (ipv4_hdr->next_proto_id != IPPROTO_TCP)
	{
		return NULL;
	}

	struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));

	// Calculate the offset to the HTTP payload
	int payload_offset = ((tcp_hdr->data_off & 0xf0) >> 2);

	if (payload_offset <= 0)
	{
		return NULL;
	}

	// Pointer to the HTTP payload
	char *payload = (char *)tcp_hdr + payload_offset;

	char *host_start = strstr(payload, "Host:");
	if (host_start != NULL)
	{
		// Increment the pointer to skip "Host: "
		host_start += 6;
		char *host_end = strchr(host_start, '\r');
		if (host_end != NULL)
		{
			// Calculate host length
			int host_length = host_end - host_start;

			// Allocate memory for host
			char *host = (char *)malloc((host_length + 1) * sizeof(char)); // +1 for null terminator
			if (host == NULL)
			{
				return NULL;
			}

			// Copy host from payload
			strncpy(host, host_start, host_length);
			host[host_length] = '\0'; // Null-terminate the string
			return host;
		}
	}

	return NULL; // Return NULL if the HTTP host is not found or an error occurs.
}

/**
 * This function extracts the domain name from a packet based on the protocol.
 *
 * @param pkt A pointer to the packet from which to extract the domain name.
 * @param protocol The protocol of the packet (HTTP or HTTPS).
 * @return A pointer to the extracted domain name.
 */
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

		// Set TCP header length
		tcp_hdr->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4; // Divide by 4 for 32-bit words

		// Set TCP flags to reset (RST)
		tcp_hdr->tcp_flags = RTE_TCP_RST;

		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));

		// Extract the acknowledgment number from the TCP header
		uint32_t ack_number = rte_be_to_cpu_32(tcp_hdr->recv_ack);

		// Set the sequence number in the TCP header to the received acknowledgment number
		tcp_hdr->sent_seq = rte_cpu_to_be_32(ack_number);

		tcp_hdr->recv_ack = 0;

		// Calculate and set the new IP and TCP checksums (optional)
		tcp_hdr->cksum = 0;
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);

		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
	}
}

/**
 * This function resets the TCP server by sending a TCP RST packet in response to a received packet.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline void reset_tcp_server(struct rte_mbuf *rx_pkt)
{
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// Increment the sequence number by 1
		tcp_hdr->sent_seq = rte_cpu_to_be_32(rte_be_to_cpu_32(tcp_hdr->sent_seq) + 1);

		// Set acknowledgment number to 0
		tcp_hdr->recv_ack = rte_cpu_to_be_32(0);

		// Set TCP flags to reset (RST)
		tcp_hdr->tcp_flags = RTE_TCP_RST;

		// Set TCP header length
		tcp_hdr->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4; // Divide by 4 for 32-bit words

		// Set IP total length to 40 (TCP RST packets have no payload)
		ip_hdr->total_length = rte_cpu_to_be_16(40);

		// Calculate and set the new IP and TCP checksums
		ip_hdr->hdr_checksum = 0;
		tcp_hdr->cksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
void countStrings(char strings[CACHE_SIZE][MAX_STRINGS], int numStrings)
{

	for (int i = 0; i < numStrings; i++)
	{
		// Use a flag to track if the string is found
		int found = 0;

		// Iterate over the existing db_hit_count entries
		for (int j = 0; j < CACHE_SIZE; j++)
		{
			// If the string is found, increment the hit_count
			if (strcmp(strings[i], db_hit_count[j].id) == 0)
			{
				db_hit_count[j].hit_count++;
				found = 1;
				break;
			}
			// If an empty slot is found, add the string as a new entry
			else if (db_hit_count[j].hit_count == 0)
			{
				strcpy(db_hit_count[j].id, strings[i]);
				db_hit_count[j].hit_count = 1;
				found = 1;
				break;
			}
		}

		// If the string wasn't found and no empty slots are available, stop
		if (!found)
		{
			break;
		}
	}
}

/**
 * Calculates the sum of hit counts and sends the data to the server at a specified time interval.
 *
 * @param last_run_count Pointer to the last run count value.
 * @param jsonArray Pointer to the JSON array object.
 */

static void sum_count(int *last_run_count, json_t *jsonArray)
{
	time_t rawtime;
	struct tm *timeinfo;
	char timestamp[20];
	time(&rawtime);
	timeinfo = localtime(&rawtime);

	countStrings(hitCount, hitCounter);
	memset(hitCount, 0, sizeof(hitCount));
	hitCounter = 0;

	int current_min = timeinfo->tm_min;
	if (current_min % TIMER_PERIOD_SEND == 0 && current_min != *last_run_count)
	{
		populate_json_hitcount(jsonArray);

		send_hitcount_to_server(jsonArray);

		*last_run_count = current_min;
	}
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
void init_database()
{
	// Check if the database file exists
	if (access(db_path, F_OK) != -1)
	{
		// Database file exists, open it
		if (sqlite3_open(db_path, &db) != SQLITE_OK)
		{
			// Handle database opening error
			logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error opening the database: %s\n", sqlite3_errmsg(db));
			// You may want to exit or return an error code here
		}
	}
	else
	{
		// Database file does not exist, create it
		if (sqlite3_open(db_path, &db) != SQLITE_OK)
		{
			// Handle database creation error
			logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error creating the database: %s\n", sqlite3_errmsg(db));
			// You may want to exit or return an error code here
		}
	}

	// Check if the 'policies' table exists
	char *check_table_sql = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='policies';";
	sqlite3_stmt *stmt;
	int result = sqlite3_prepare_v2(db, check_table_sql, -1, &stmt, NULL);

	if (result == SQLITE_OK)
	{
		if (sqlite3_step(stmt) == SQLITE_ROW)
		{
			int table_count = sqlite3_column_int(stmt, 0);
			if (table_count == 0)
			{
				// 'policies' table does not exist, create it
				char *create_table_sql = "CREATE TABLE policies (id TEXT PRIMARY KEY, ip_address TEXT, domain TEXT);";
				if (sqlite3_exec(db, create_table_sql, NULL, 0, NULL) != SQLITE_OK)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error creating the 'policies' table: %s\n", sqlite3_errmsg(db));
					// You may want to exit or return an error code here
				}
				else
				{
					logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Created 'policies' table.\n");
				}
			}
			else
			{
				logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "'policies' table already exists.\n");
			}
		}
	}
	else
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error checking for 'policies' table: %s\n", sqlite3_errmsg(db));
	}

	sqlite3_finalize(stmt); // Finalize the prepared statement
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
void delete_database()
{
	// Close the database if it's open
	if (db)
	{
		sqlite3_close(db);
		db = NULL;
	}

	// Delete the database file
	if (remove(db_path) != 0)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error deleting the database file.\n");
		return;
	}

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Database file deleted successfully.\n");
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
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
	for (int i = 0; i < ipCacheSize; ++i)
	{
		if (strcmp(ip_cache[i].ip_address, dest_ip_str) == 0)
		{
			// Found in cache, update hit count and return true
			strncpy(hitCount[hitCounter], ip_cache[i].id, sizeof(ip_cache[i].id));
			hitCounter++;
			return true;
		}
	}

	// Prepare an SQL query to check if the IP address exists in the database
	char query[256];
	snprintf(query, sizeof(query), "SELECT id FROM policies WHERE ip_address = '%s'", dest_ip_str);

	// Execute the SQL query
	sqlite3_stmt *stmt;
	int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

	if (result != SQLITE_OK)
	{
		return false; // Error in preparing statement
	}

	// Execute the query and check if the IP address exists in the database
	char id[MAX_STRINGS] = {0}; // Assuming ID is a string of 36 characters
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		// Retrieve the ID from the result
		strncpy(id, (const char *)sqlite3_column_text(stmt, 0), sizeof(id));
	}

	// Finalize the statement
	sqlite3_finalize(stmt);

	if (strlen(id) > 0)
	{
		// Add to cache
		if (ipCacheSize < CACHE_SIZE)
		{
			strncpy(ip_cache[ipCacheSize].ip_address, dest_ip_str, sizeof(ip_cache[ipCacheSize].ip_address));
			strncpy(ip_cache[ipCacheSize].id, id, sizeof(ip_cache[ipCacheSize].id));
			ipCacheSize++;
		}

		// Update hit count
		strncpy(hitCount[hitCounter], id, sizeof(id));
		hitCounter++;
		return true;
	}

	return false;
}

// Hash function
unsigned int hash_function(const char *str)
{
	unsigned int hash = 5381;
	int c;

	while ((c = *str++))
	{
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
	}

	return hash % CACHE_SIZE;
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline bool domain_checker(char *domain)
{

	if (domain == NULL || domain[0] == '\0')
	{
		return false;
	}

	// Use a hash table for faster lookup in the cache
	unsigned int hash = hash_function(domain);
	for (int i = 0; i < domainCacheSize; ++i)
	{
		if (strcmp(domain_cache[hash].domain, domain) == 0)
		{
			// Found in cache, update hit count and return true
			if (hitCounter < CACHE_SIZE)
			{
				strncpy(hitCount[hitCounter], domain_cache[hash].id, sizeof(domain_cache[hash].id));
				hitCounter++;
			}
			else
			{
				countFlag = 1;
				logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to update Hitcount\n");
			}

			return true;
		}
		hash = (hash + 1) % CACHE_SIZE; // Linear probing for collision resolution
	}

	if (!db)
	{
		// Database is not initialized
		return false;
	}

	// Prepare an SQL query to check if the domain exists in the database
	char query[256];
	snprintf(query, sizeof(query), "SELECT id FROM policies WHERE domain = '%s'", domain);

	// Execute the SQL query
	sqlite3_stmt *stmt;
	int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

	if (result != SQLITE_OK)
	{
		return false; // Error in preparing statement
	}

	// Execute the query and check if the domain exists in the database
	char id[MAX_STRINGS] = {0}; // Assuming ID is a string of 36 characters
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		// Retrieve the ID from the result
		strncpy(id, (const char *)sqlite3_column_text(stmt, 0), sizeof(id));
	}

	// Finalize the statement
	sqlite3_finalize(stmt);

	if (strlen(id) > 0)
	{
		// Add to cache
		if (domainCacheSize < CACHE_SIZE)
		{
			strncpy(domain_cache[hash].domain, domain, sizeof(domain_cache[hash].domain));
			strncpy(domain_cache[hash].id, id, sizeof(domain_cache[hash].id));
			domainCacheSize++;
		}

		// Update hit count
		if (hitCounter < CACHE_SIZE)
		{
			strncpy(hitCount[hitCounter], id, sizeof(id));
			hitCounter++;
		}
		else
		{
			countFlag = 1;
			logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Failed to update Hitcount\n");
		}

		return true;
	}

	return false;
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline void
lcore_stats_process(void)
{
	// Variable declaration
	int last_run_stat = 0; // lastime statistics printed
	int last_run_file = 0; // lastime statistics printed to file
	int last_run_send = 0;
	int last_run_print = 0;
	int last_run_hitcount = 0; // lastime statistics sent to server

	FILE *f_stat = NULL; // File pointer for statistics
	json_t *jsonStats = json_array();
	json_t *jsonHitcount = json_array(); // JSON array for statistics

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Starting stats process in %d\n", rte_lcore_id());

	while (!force_quit)
	{
		sum_count(&last_run_hitcount, jsonHitcount);

		print_stats_file(&last_run_stat, &last_run_file, &f_stat, jsonStats);

		print_stats(&last_run_print);
		// Send stats
		send_stats(jsonStats, &last_run_send);
		// Print the statistics
	}
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline void
lcore_http_process(void)
{
	uint16_t port;
	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
	if (rte_eth_dev_socket_id(port) >= 0 &&
		rte_eth_dev_socket_id(port) !=
			(int)rte_socket_id())
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "WARNING, port %u is on remote NUMA node to "
													   "polling thread.\n\tPerformance will "
													   "not be optimal.\n",
				   port);

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Core %u forwarding packets. [Ctrl+C to quit]\n",
			   rte_lcore_id());

	struct rte_mbuf *rx_bufs[BURST_SIZE];
	// Main work of application loop
	while (!force_quit)
	{

		/* Get a burst of RX packets from the first port of the pair. */

		const uint16_t rx_count = rte_eth_rx_burst(0, 0, rx_bufs, BURST_SIZE);

		for (uint16_t i = 0; i < rx_count; i++)
		{
			struct rte_mbuf *rx_pkt = rx_bufs[i];

			if (domain_checker(extractDomainfromHTTP(rx_pkt)))
			{
				// Create a copy of the received packet
				struct rte_mbuf *rst_pkt_client = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
				if (rst_pkt_client == NULL)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error copying packet to RST Client\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet                // Skip this packet
				}
				struct rte_mbuf *rst_pkt_server = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
				if (rst_pkt_server == NULL)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error copying packet to RST Server\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet
				}

				// Apply modifications to the packets
				reset_tcp_client(rst_pkt_client);
				reset_tcp_server(rst_pkt_server);

				// Transmit modified packets
				const uint16_t rst_client_tx_count = rte_eth_tx_burst(2, 0, &rst_pkt_client, 1);
				if (rst_client_tx_count == 0)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error sending packet to client\n");
					rte_pktmbuf_free(rst_pkt_client); // Free the modified packet
				}
				else
				{
					rstClient_http++;
				}

				const uint16_t rst_server_tx_count = rte_eth_tx_burst(2, 0, &rst_pkt_server, 1);
				if (rst_server_tx_count == 0)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error sending packet to server\n");
					rte_pktmbuf_free(rst_pkt_server); // Free the modified packet
				}
				else
				{
					rstServer_http++;
				}
			}

			rte_pktmbuf_free(rx_pkt); // Free the original packet
		}
	}
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline void
lcore_https_process(void)
{
	// initialization
	char *extractedName;
	uint16_t port;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
	if (rte_eth_dev_socket_id(port) >= 0 &&
		rte_eth_dev_socket_id(port) !=
			(int)rte_socket_id())
		logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "WARNING, port %u is on remote NUMA node to "
													   "polling thread.\n\tPerformance will "
													   "not be optimal.\n",
				   port);

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Core %u forwarding packets. [Ctrl+C to quit]\n",
			   rte_lcore_id());

	struct rte_mbuf *rx_bufs[BURST_SIZE];
	// Main work of application loop
	while (!force_quit)
	{

		/* Get a burst of RX packets from the first port of the pair. */

		const uint16_t rx_count = rte_eth_rx_burst(1, 0, rx_bufs, BURST_SIZE);

		for (uint16_t i = 0; i < rx_count; i++)
		{
			struct rte_mbuf *rx_pkt = rx_bufs[i];

			if (domain_checker(extractDomainfromHTTPS(rx_pkt)))
			{
				// Create a copy of the received packet
				struct rte_mbuf *rst_pkt_client = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
				if (rst_pkt_client == NULL)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error copying packet to RST Client\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet                // Skip this packet
				}
				struct rte_mbuf *rst_pkt_server = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
				if (rst_pkt_server == NULL)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error copying packet to RST Server\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet
				}

				// Apply modifications to the packets
				reset_tcp_client(rst_pkt_client);
				reset_tcp_server(rst_pkt_server);

				// Transmit modified packets
				const uint16_t rst_client_tx_count = rte_eth_tx_burst(3, 0, &rst_pkt_client, 1);
				if (rst_client_tx_count == 0)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error sending packet to client\n");
					rte_pktmbuf_free(rst_pkt_client); // Free the modified packet
				}
				else
				{
					rstClient_tls++;
				}

				const uint16_t rst_server_tx_count = rte_eth_tx_burst(3, 0, &rst_pkt_server, 1);
				if (rst_server_tx_count == 0)
				{
					logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error sending packet to server\n");
					rte_pktmbuf_free(rst_pkt_server); // Free the modified packet
				}
				else
				{
					rstServer_tls++;
				}
			}

			rte_pktmbuf_free(rx_pkt); // Free the original packet
		}
	}
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
static inline void
lcore_heartbeat_process()
{
	CURL *curl;
	CURLcode res;
	char post_fields[256];
	char url[256];
	char timestamp_str[25];
	time_t timestamp;
	struct tm *tm_info;
	struct curl_slist *headers = NULL;

	sprintf(url, "%s/ps/heartbeat", HOSTNAME);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();

	if (curl)
	{
		headers = curl_slist_append(headers, "Content-Type: application/json");

		while (!force_quit)
		{
			timestamp = time(NULL);
			tm_info = gmtime(&timestamp);
			strftime(timestamp_str, 25, "%Y-%m-%dT%H:%M:%S.000Z", tm_info);

			sprintf(post_fields, "[{\"ps_id\": \"%s\", \"time\": \"%s\"}]", PS_ID, timestamp_str);

			logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Post Fields : %s\n", post_fields);
			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

			res = curl_easy_perform(curl);

			if (res != CURLE_OK)
			{
				logMessage(LOG_LEVEL_WARNING, __FILE__, __LINE__, "Heartbeat failed: %s\n", curl_easy_strerror(res));
			}
			sleep(5);
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
}

/**
 * This function creates a Kafka consumer, subscribes to a Kafka topic, opens an SQLite database,
 * and starts consuming messages from the topic. The consumed messages are then stored in the database.
 * The function continues to consume messages until the `force_quit` flag is set to true.
 */
static inline void
lcore_sync_database()
{
	rd_kafka_t *rk;			 // Kafka handle
	rd_kafka_conf_t *conf;	 // Kafka configuration
	rd_kafka_resp_err_t err; // Kafka error handler
	rd_kafka_topic_t *topic; // Kafka topic

	// Kafka configuration
	conf = rd_kafka_conf_new();
	if (rd_kafka_conf_set(conf, "bootstrap.servers", KAFKA_BROKER, NULL, 0) != RD_KAFKA_CONF_OK)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Failed to set Kafka broker configuration\n");
		return;
	}

	// Create Kafka consumer
	rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, NULL, 0);
	if (!rk)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Failed to create Kafka consumer\n");
		return;
	}

	// Subscribe to Kafka topic
	topic = rd_kafka_topic_new(rk, KAFKA_TOPIC, NULL);
	if (!topic)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Failed to create Kafka topic object\n");
		rd_kafka_destroy(rk);
		return;
	}

	// Open SQLite database
	if (sqlite3_open(db_path, &db) != SQLITE_OK)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Can't open database: %s\n", sqlite3_errmsg(db));
		rd_kafka_topic_destroy(topic);
		rd_kafka_destroy(rk);
		return;
	}

	// Start consuming messages
	if (rd_kafka_consume_start(topic, 0, RD_KAFKA_OFFSET_BEGINNING) == -1)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Failed to start consuming messages\n");
		rd_kafka_topic_destroy(topic);
		rd_kafka_destroy(rk);
		sqlite3_close(db);
		return;
	}

	// Loop to consume messages
	while (!force_quit)
	{
		rd_kafka_message_t *rkmessage;
		rkmessage = rd_kafka_consume(topic, 0, 1000); // 1 second timeout
		if (rkmessage)
		{
			msg_consume(rkmessage, db);
			rd_kafka_message_destroy(rkmessage);
		}
	}

	// Cleanup
	rd_kafka_consume_stop(topic, 0);
	rd_kafka_topic_destroy(topic);
	rd_kafka_destroy(rk);
	sqlite3_close(db);
}

/**
 * This function is responsible for handling the received packets.
 *
 * @param rx_pkt A pointer to the received packet.
 */
int main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;
	unsigned lcore_id, lcore_http = 0, lcore_stats = 0, lcore_db = 0, lcore_https = 0;
	init_database();

	// load the config file
	if (load_config_file())
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Cannot load the config file\n");
		rte_exit(EXIT_FAILURE, "Cannot load the config file\n");
	}

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Configuration File Successfully Updated\n");

	// Initializion the Environment Abstraction Layer (EAL)
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error with EAL initialization\n");
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	}

	argc -= ret;
	argv += ret;

	// force quit handler
	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// clean the data
	memset(port_statistics, 0, 32 * sizeof(struct port_statistics_data));
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Port Statistic Cleared\n");

	// count the number of ports to send and receive
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports != 4)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Error: number of ports must be 4\n");
		rte_exit(EXIT_FAILURE, "Error: number of ports must be 4\n");
	}

	// allocates the mempool to hold the mbufs
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
										MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	// check the mempool allocation
	if (mbuf_pool == NULL)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Cannot create mbuf pool\n");
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
	}
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Successfully Created Mbuf Pool\n");

	// initializing ports
	RTE_ETH_FOREACH_DEV(portid)
	if (port_init(portid, mbuf_pool) != 0)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "Cannot init port %" PRIu16 "\n", portid);
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);
	}

	// count the number of lcore
	if (rte_lcore_count() < 5)
	{
		logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, "lcore must be more than equal 4\n");
		rte_exit(EXIT_FAILURE, "lcore must be more than equal 4\n");
	}

	RTE_LCORE_FOREACH_WORKER(lcore_id)
	{
		if (lcore_id == (unsigned int)lcore_https ||
			lcore_id == (unsigned int)lcore_http ||
			lcore_id == (unsigned int)lcore_stats ||
			lcore_id == (unsigned int)lcore_db)
		{
			continue;
		}
		if (lcore_http == 0)
		{
			lcore_http = lcore_id;
			logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "HTTP Core Assigned On Core %u\n", lcore_id);
			continue;
		}
		if (lcore_https == 0)
		{
			lcore_https = lcore_id;
			logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "HTTPS Core Assigned On Core %u\n", lcore_id);
			continue;
		}
		if (lcore_stats == 0)
		{
			lcore_stats = lcore_id;
			logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Stats Core Assigned On Core %u\n", lcore_id);
			continue;
		}
		if (lcore_db == 0)
		{
			lcore_db = lcore_id;
			logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Kafka Core Assigned On Core %u\n", lcore_id);
			continue;
		}
	}

	// run the lcore main function
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Initiating HTTP Core\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_http_process,
						  NULL, lcore_http);

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Initiating HTTPS Core\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_https_process,
						  NULL, lcore_https);

	// run the stats
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Initiating Statistic Core\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_stats_process,
						  NULL, lcore_stats);

	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Initiating Kafka Core\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_sync_database,
						  NULL, lcore_db);

	// run the heartbeat
	logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, "Initiating Heartbeat Mechanism\n");
	lcore_heartbeat_process();

	// wait all lcore stopped
	RTE_LCORE_FOREACH_WORKER(lcore_id)
	{
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}
	// clean up the EAL
	delete_database();
	rte_eal_cleanup();
}