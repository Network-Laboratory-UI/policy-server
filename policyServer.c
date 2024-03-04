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

#define CACHE_SIZE 1000
#define RTE_TCP_RST 0x04
#define KAFKA_TOPIC "dpdk-blocked-list"
#define KAFKA_BROKER "192.168.0.90:9092"

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
static sqlite3 *db;
static volatile bool force_quit;
static struct IP_Cache ip_cache[CACHE_SIZE];
static struct DomainCacheEntry domain_cache[CACHE_SIZE];

struct IP_Cache
{
	char ip[INET_ADDRSTRLEN];
	bool exists;
};
struct DomainCacheEntry
{
	char domain[256];
	bool exists;
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

void logMessage(const char *filename, int line, const char *format, ...)
{
	// Open the log file in append mode
	FILE *file = fopen("logs/log.txt", "a");
	if (file == NULL)
	{
		logMessage(__FILE__, __LINE__, "Error opening file %s\n", filename);
		return;
	}

	// Get the current time
	time_t rawtime;
	struct tm *timeinfo;
	char timestamp[20];
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

	// Write the timestamp to the file
	// fprintf(file, "[%s] ", timestamp);
	fprintf(file, "[%s] [%s:%d] - ", timestamp, filename, line);

	// Write the formatted message to the file
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);

	// Close the file
	fclose(file);
}
void msg_consume(rd_kafka_message_t *rkmessage, sqlite3 *db)
{
	if (rkmessage->err)
	{
		logMessage(__FILE__, __LINE__, "Kafka error: %s\n", rd_kafka_message_errstr(rkmessage));
		return;
	}

	logMessage(__FILE__, __LINE__, "Received message: %.*s\n", (int)rkmessage->len, (char *)rkmessage->payload);

	// Parse JSON message
	json_error_t error;
	json_t *root = json_loadb(rkmessage->payload, rkmessage->len, 0, &error);
	if (!root)
	{
		logMessage(__FILE__, __LINE__, "JSON parsing error: %s\n", error.text);
		return;
	}

	const char *type_str = NULL;
	const char *createdBlockedListKey = NULL;

	// Check 'type' field
	json_t *type = json_object_get(root, "type");
	if (!type || !json_is_string(type))
	{
		logMessage(__FILE__, __LINE__, "Key 'type' not found or not a string\n");
		goto cleanup;
	}
	type_str = json_string_value(type);
	if (!type_str)
	{
		logMessage(__FILE__, __LINE__, "Failed to get 'type' value as string\n");
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
		logMessage(__FILE__, __LINE__, "Unsupported 'type' value: %s\n", type_str);
		goto cleanup;
	}

	// Extract 'createdBlockedList'
	json_t *createdBlockedList = json_object_get(root, createdBlockedListKey);
	if (!createdBlockedList || !json_is_object(createdBlockedList))
	{
		logMessage(__FILE__, __LINE__, "Key '%s' not found or not an object\n", createdBlockedListKey);
		goto cleanup;
	}

	// Extract 'domain', 'ip_add', and 'id' fields
	json_t *domain = json_object_get(createdBlockedList, "domain");
	json_t *ip_add = json_object_get(createdBlockedList, "ip_add");
	json_t *id = json_object_get(createdBlockedList, "id");

	if (!domain || !json_is_string(domain) || !ip_add || !json_is_string(ip_add) || !id || !json_is_string(id))
	{
		logMessage(__FILE__, __LINE__, "Missing or invalid keys in 'createdBlockedList'\n");
		goto cleanup;
	}

	const char *domain_str = json_string_value(domain);
	const char *ip_str = json_string_value(ip_add);
	const char *id_str = json_string_value(id);

	if (!domain_str || !ip_str || !id_str)
	{
		logMessage(__FILE__, __LINE__, "Failed to get values from 'createdBlockedList'\n");
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
		memset(ip_cache, 0, sizeof(ip_cache));
		memset(domain_cache, 0, sizeof(domain_cache));
	}
	else if (strcmp(type_str, "delete") == 0)
	{
		query_len = snprintf(sql_query, sizeof(sql_query), "DELETE FROM policies WHERE id='%s';", id_str);
		memset(ip_cache, 0, sizeof(ip_cache));
		memset(domain_cache, 0, sizeof(domain_cache));
	}
	else
	{
		logMessage(__FILE__, __LINE__, "Unsupported 'type' value: %s\n", type_str);
		goto cleanup;
	}

	if (query_len <= 0 || query_len >= sizeof(sql_query))
	{
		logMessage(__FILE__, __LINE__, "SQL query creation error\n");
		goto cleanup;
	}

	char *errmsg = NULL;
	if (sqlite3_exec(db, sql_query, NULL, 0, &errmsg) != SQLITE_OK)
	{
		logMessage(__FILE__, __LINE__, "SQL error: %s\n", errmsg);
		sqlite3_free(errmsg);
	}
	else
	{
		logMessage(__FILE__, __LINE__, "%s updated to the database\n", domain_str);
	}

cleanup:
	json_decref(root);
}

// Function to set up Kafka consumer
void run_kafka_consumer()
{
	rd_kafka_t *rk;			 // Kafka handle
	rd_kafka_conf_t *conf;	 // Kafka configuration
	rd_kafka_resp_err_t err; // Kafka error handler
	rd_kafka_topic_t *topic; // Kafka topic

	// Kafka configuration
	conf = rd_kafka_conf_new();
	if (rd_kafka_conf_set(conf, "bootstrap.servers", KAFKA_BROKER, NULL, 0) != RD_KAFKA_CONF_OK)
	{
		logMessage(__FILE__, __LINE__, "Failed to set Kafka broker configuration\n");
		return;
	}

	// Create Kafka consumer
	rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, NULL, 0);
	if (!rk)
	{
		logMessage(__FILE__, __LINE__, "Failed to create Kafka consumer\n");
		return;
	}

	// Subscribe to Kafka topic
	topic = rd_kafka_topic_new(rk, KAFKA_TOPIC, NULL);
	if (!topic)
	{
		logMessage(__FILE__, __LINE__, "Failed to create Kafka topic object\n");
		rd_kafka_destroy(rk);
		return;
	}

	// Open SQLite database
	if (sqlite3_open(db_path, &db) != SQLITE_OK)
	{
		logMessage(__FILE__, __LINE__, "Can't open database: %s\n", sqlite3_errmsg(db));
		rd_kafka_topic_destroy(topic);
		rd_kafka_destroy(rk);
		return;
	}

	// Start consuming messages
	if (rd_kafka_consume_start(topic, 0, RD_KAFKA_OFFSET_BEGINNING) == -1)
	{
		logMessage(__FILE__, __LINE__, "Failed to start consuming messages\n");
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
		logMessage(__FILE__, __LINE__, "Error during getting device (port %u) info: %s\n",
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

	logMessage(__FILE__, __LINE__, "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			   port, RTE_ETHER_ADDR_BYTES(&addr));

	// SET THE PORT TO PROMOCIOUS
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static FILE *open_file(const char *filename)
{
	logMessage(__FILE__, __LINE__, "Opening file %s\n", filename);
	FILE *f = fopen(filename, "a+");
	if (f == NULL)
	{
		logMessage(__FILE__, __LINE__, "Error opening file %s\n", filename);
		rte_exit(EXIT_FAILURE, "Error opening file %s\n", filename);
	}
	return f;
}

/*
 * The print statistics function
 * Print the statistics to the console
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

		for (portid = 0; portid < 2; portid++)
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
		*last_run_print = timeinfo->tm_sec;
	}
}

static void print_stats_csv_header(FILE *f)
{
	fprintf(f, "ps_id,rstClient,rstServer,rx_0_count,tx_0_count,rx_0_size,tx_0_size,rx_0_drop,rx_0_error,tx_0_error,rx_0_mbuf,rx_1_count,tx_1_count,rx_1_size,tx_1_size,rx_1_drop,rx_1_error,tx_1_error,rx_1_mbuf,time,throughput\n"); // Header row
}

static void print_stats_csv(FILE *f, char *timestamp)
{
	// Write data to the CSV file
	fprintf(f, "%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%s,%ld\n", PS_ID, port_statistics[1].rstClient, port_statistics[1].rstServer, port_statistics[0].rx_count, port_statistics[0].tx_count, port_statistics[0].rx_size, port_statistics[0].tx_size, port_statistics[0].dropped, port_statistics[0].err_rx, port_statistics[0].err_tx, port_statistics[0].mbuf_err, port_statistics[1].rx_count, port_statistics[1].tx_count, port_statistics[1].rx_size, port_statistics[1].tx_size, port_statistics[1].dropped, port_statistics[1].err_rx, port_statistics[1].err_tx, port_statistics[1].mbuf_err, timestamp, port_statistics[1].throughput);
}

static void clear_stats(void)
{
	memset(port_statistics, 0, RTE_MAX_ETHPORTS * sizeof(struct port_statistics_data));
}

/*
 * The load configuration file function
 * Load the configuration file
 */
int load_config_file()
{
	FILE *configFile = fopen("config/config.cfg", "r");
	if (configFile == NULL)
	{
		logMessage(__FILE__, __LINE__, "Cannot open the config file\n");
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
				logMessage(__FILE__, __LINE__, "MAX_PACKET_LEN: %d\n", MAX_PACKET_LEN);
			}
			else if (strcmp(key, "RX_RING_SIZE") == 0)
			{
				RX_RING_SIZE = atoi(value);
				logMessage(__FILE__, __LINE__, "RX_RING_SIZE: %d\n", RX_RING_SIZE);
			}
			else if (strcmp(key, "TX_RING_SIZE") == 0)
			{
				TX_RING_SIZE = atoi(value);
				logMessage(__FILE__, __LINE__, "TX_RING_SIZE: %d\n", TX_RING_SIZE);
			}
			else if (strcmp(key, "NUM_MBUFS") == 0)
			{
				NUM_MBUFS = atoi(value);
				logMessage(__FILE__, __LINE__, "NUM_MBUFS: %d\n", NUM_MBUFS);
			}
			else if (strcmp(key, "MBUF_CACHE_SIZE") == 0)
			{
				MBUF_CACHE_SIZE = atoi(value);
				logMessage(__FILE__, __LINE__, "MBUF_CACHE_SIZE: %d\n", MBUF_CACHE_SIZE);
			}
			else if (strcmp(key, "BURST_SIZE") == 0)
			{
				BURST_SIZE = atoi(value);
				logMessage(__FILE__, __LINE__, "BURST_SIZE: %d\n", BURST_SIZE);
			}
			else if (strcmp(key, "MAX_TCP_PAYLOAD_LEN") == 0)
			{
				MAX_TCP_PAYLOAD_LEN = atoi(value);
				logMessage(__FILE__, __LINE__, "MAX_TCP_PAYLOAD_LEN: %d\n", MAX_TCP_PAYLOAD_LEN);
			}
			else if (strcmp(key, "STAT_FILE") == 0)
			{
				strcpy(STAT_FILE, value);
				logMessage(__FILE__, __LINE__, "STAT_FILE: %s\n", STAT_FILE);
			}
			else if (strcmp(key, "STAT_FILE_EXT") == 0)
			{
				strcpy(STAT_FILE_EXT, value);
				logMessage(__FILE__, __LINE__, "STAT_FILE_EXT: %s\n", STAT_FILE_EXT);
			}
			else if (strcmp(key, "TIMER_PERIOD_STATS") == 0)
			{
				TIMER_PERIOD_STATS = atoi(value);
				logMessage(__FILE__, __LINE__, "TIMER_PERIOD_STATS: %d\n", TIMER_PERIOD_STATS);
			}
			else if (strcmp(key, "TIMER_PERIOD_SEND") == 0)
			{
				TIMER_PERIOD_SEND = atoi(value);
				logMessage(__FILE__, __LINE__, "TIMER_PERIOD_SEND: %d\n", TIMER_PERIOD_SEND);
			}
			else if (strcmp(key, "ID_PS") == 0)
			{
				strcpy(PS_ID, value);
				logMessage(__FILE__, __LINE__, "PS ID: %s\n", PS_ID);
			}
			else if (strcmp(key, "HOSTNAME") == 0)
			{
				strcpy(HOSTNAME, value);
				logMessage(__FILE__, __LINE__, "HOSTNAME: %s\n", HOSTNAME);
			}
		}
	}

	fclose(configFile);
	return 0;
}

/*
 * The termination signal handler
 * Handle the termination signal
 * @param signum
 * 	the signal number
 */
static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		logMessage(__FILE__, __LINE__, "Signal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

static void
populate_json_array(json_t *jsonArray, char *timestamp)
{
	// Create object for the statistics
	json_t *jsonObject = json_object();

	// Populate the JSON object
	json_object_set(jsonObject, "ps_id", json_string(PS_ID));
	json_object_set(jsonObject, "rx_0_count", json_integer(port_statistics[0].rx_count));
	json_object_set(jsonObject, "tx_0_count", json_integer(port_statistics[0].tx_count));
	json_object_set(jsonObject, "rx_0_size", json_integer(port_statistics[0].rx_size));
	json_object_set(jsonObject, "tx_0_size", json_integer(port_statistics[0].tx_size));
	json_object_set(jsonObject, "rx_0_drop", json_integer(port_statistics[0].dropped));
	json_object_set(jsonObject, "rx_0_error", json_integer(port_statistics[0].err_rx));
	json_object_set(jsonObject, "tx_0_error", json_integer(port_statistics[0].err_tx));
	json_object_set(jsonObject, "rx_0_mbuf", json_integer(port_statistics[0].mbuf_err));
	json_object_set(jsonObject, "rstClient", json_integer(port_statistics[1].rstClient));
	json_object_set(jsonObject, "rstServer", json_integer(port_statistics[1].rstServer));
	json_object_set(jsonObject, "rx_1_count", json_integer(port_statistics[1].rx_count));
	json_object_set(jsonObject, "tx_1_count", json_integer(port_statistics[1].tx_count));
	json_object_set(jsonObject, "rx_1_size", json_integer(port_statistics[1].rx_size));
	json_object_set(jsonObject, "tx_1_size", json_integer(port_statistics[1].tx_size));
	json_object_set(jsonObject, "rx_1_drop", json_integer(port_statistics[1].dropped));
	json_object_set(jsonObject, "rx_1_error", json_integer(port_statistics[1].err_rx));
	json_object_set(jsonObject, "tx_1_error", json_integer(port_statistics[1].err_tx));
	json_object_set(jsonObject, "rx_1_mbuf", json_integer(port_statistics[1].mbuf_err));
	json_object_set(jsonObject, "time", json_string(timestamp));
	json_object_set(jsonObject, "throughput", json_integer(port_statistics[0].throughput));

	// Append the JSON object to the JSON array
	json_array_append(jsonArray, jsonObject);
}

static void
collect_stats()
{
	// Get the statistics
	rte_eth_stats_get(1, &stats_1);
	rte_eth_stats_get(0, &stats_0);

	// Update the statistics
	port_statistics[1].rx_count = stats_1.ipackets;
	port_statistics[1].tx_count = stats_1.opackets;
	port_statistics[1].rx_size = stats_1.ibytes;
	port_statistics[1].tx_size = stats_1.obytes;
	port_statistics[1].dropped = stats_1.imissed;
	port_statistics[1].err_rx = stats_1.ierrors;
	port_statistics[1].err_tx = stats_1.oerrors;
	port_statistics[1].mbuf_err = stats_1.rx_nombuf;
	port_statistics[0].rx_count = stats_0.ipackets;
	port_statistics[0].tx_count = stats_0.opackets;
	port_statistics[0].rx_size = stats_0.ibytes;
	port_statistics[0].tx_size = stats_0.obytes;
	port_statistics[0].dropped = stats_0.imissed;
	port_statistics[0].err_rx = stats_0.ierrors;
	port_statistics[0].err_tx = stats_0.oerrors;
	port_statistics[0].mbuf_err = stats_0.rx_nombuf;

	// Clear the statistics
	rte_eth_stats_reset(0);
	rte_eth_stats_reset(1);

	// Calculate the throughput
	port_statistics[1].throughput = port_statistics[1].rx_size / TIMER_PERIOD_STATS;
	port_statistics[0].throughput = port_statistics[0].tx_size / TIMER_PERIOD_STATS;
}
/*
 * The print statistics file function
 * Print the statistics to the file
 * @param last_run_stat
 * 	the last run statistics
 * @param last_run_file
 * 	the last run file
 * @param f_stat
 * 	the file pointer
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
		populate_json_array(jsonArray, time_str);
		fflush(*f_stat);

		// clear the stats
		clear_stats();

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

static void
send_stats_to_server(json_t *jsonArray)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = curl_slist_append(headers, "Content-Type: application/json");
	;
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
			logMessage(__FILE__, __LINE__, "Send Stats failed: %s\n", curl_easy_strerror(res));
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(jsonString);
		json_array_clear(jsonArray);
	}

	curl_global_cleanup();
}

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
		logMessage(__FILE__, __LINE__, "Sending statistics to server\n");
		send_stats_to_server(jsonArray);
		*last_run_send = current_min;
	}
}

static inline char *extractDomainfromHTTPS(struct rte_mbuf *pkt)
{
	// Extract Ethernet header
	struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
	{
		logMessage(__FILE__, __LINE__, "Packet is not an IPv4 packet\n");
		return NULL;
	}

	// Extract IPv4 header
	struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

	if (ip_hdr->next_proto_id != IPPROTO_TCP)
	{
		logMessage(__FILE__, __LINE__, "Packet is not a TCP packet\n");
		return NULL;
	}

	// Extract TCP header
	struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + sizeof(struct rte_ipv4_hdr));

	// Calculate the offset to the TLS header (if TLS is in use)
	int tls_offset = (tcp_hdr->data_off & 0xf0) >> 2;

	if (tls_offset <= 0)
	{
		logMessage(__FILE__, __LINE__, "No TLS header found in the packet\n");
		return NULL;
	}

	// Calculate the total length of the TLS payload
	int tls_payload_length = ntohs(ip_hdr->total_length) - (sizeof(struct rte_ipv4_hdr) + (tcp_hdr->data_off >> 4) * 4);

	if (tls_payload_length <= 0)
	{
		logMessage(__FILE__, __LINE__, "No TLS payload found in the packet\n");
		return NULL;
	}

	int start_offset = 76;
	int end_offset = 77;

	if (start_offset < 0 || end_offset >= tls_payload_length)
	{
		logMessage(__FILE__, __LINE__, "Invalid byte range specified for the TLS payload\n");
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
		logMessage(__FILE__, __LINE__, "No HTTP payload found in the packet\n");
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
				return NULL;
			}
			int host_length = host_end - host_start - 6; // Subtract "Host: "
			if (host_length > 0 && host_length < 256)
			{
				strncpy(host, host_start + 6, host_length);
				host[host_length] = '\0'; // Null-terminate the string
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
			logMessage(__FILE__, __LINE__, "Error opening the database: %s\n", sqlite3_errmsg(db));
			// You may want to exit or return an error code here
		}
	}
	else
	{
		// Database file does not exist, create it
		if (sqlite3_open(db_path, &db) != SQLITE_OK)
		{
			// Handle database creation error
			logMessage(__FILE__, __LINE__, "Error creating the database: %s\n", sqlite3_errmsg(db));
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
					logMessage(__FILE__, __LINE__, "Error creating the 'policies' table: %s\n", sqlite3_errmsg(db));
					// You may want to exit or return an error code here
				}
				else
				{
					logMessage(__FILE__, __LINE__, "Created 'policies' table.\n");
				}
			}
			else
			{
				logMessage(__FILE__, __LINE__, "'policies' table already exists.\n");
			}
		}
	}
	else
	{
		logMessage(__FILE__, __LINE__, "Error checking for 'policies' table: %s\n", sqlite3_errmsg(db));
		// You may want to exit or return an error code here
	}

	sqlite3_finalize(stmt); // Finalize the prepared statement
}

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
		logMessage(__FILE__, __LINE__, "Error deleting the database file.\n");
		return;
	}

	logMessage(__FILE__, __LINE__, "Database file deleted successfully.\n");
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
		logMessage(__FILE__, __LINE__, "Error preparing SQL query: %s\n", sqlite3_errmsg(db));
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

	if (count > 0)
	{
		logMessage(__FILE__, __LINE__, "IP: %s has been blocked\n", dest_ip_str);
		return true;
	}
	return false;
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

	// Check the cache first
	for (int i = 0; i < CACHE_SIZE; i++)
	{
		if (domain_cache[i].exists && strcmp(domain, domain_cache[i].domain) == 0)
		{
			return true; // Domain found in cache
		}
	}

	// Prepare an SQL query to check if the domain exists in the database
	char query[256];
	snprintf(query, sizeof(query), "SELECT COUNT(*) FROM policies WHERE domain = '%s'", domain);

	// Execute the SQL query
	sqlite3_stmt *stmt;
	int result = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);

	if (result != SQLITE_OK)
	{
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

	// Cache the result
	for (int i = 0; i < CACHE_SIZE; i++)
	{
		if (!domain_cache[i].exists)
		{
			strncpy(domain_cache[i].domain, domain, sizeof(domain_cache[i].domain) - 1);
			domain_cache[i].domain[sizeof(domain_cache[i].domain) - 1] = '\0'; // Ensure null-terminated
			domain_cache[i].exists = (count > 0);
			break;
		}
	}

	if (count > 0)
	{
		logMessage(__FILE__, __LINE__, "Domain: %s has been blocked\n", domain);
		return true;
	}
	return false;
}

static inline void
lcore_stats_process(void)
{
	// Variable declaration
	int last_run_stat = 0; // lastime statistics printed
	int last_run_file = 0; // lastime statistics printed to file
	int last_run_send = 0;
	int last_run_print = 0;							 // lastime statistics sent to server
	uint64_t start_tx_size_0 = 0, end_tx_size_0 = 0; // For throughput calculation
	uint64_t start_rx_size_1 = 0, end_rx_size_1 = 0; // For throughput calculation
	double throughput_0 = 0.0, throughput_1 = 0.0;	 // For throughput calculation
	FILE *f_stat = NULL;							 // File pointer for statistics
	json_t *jsonArray = json_array();				 // JSON array for statistics

	logMessage(__FILE__, __LINE__, "Starting stats process in %d\n", rte_lcore_id());

	while (!force_quit)
	{
		print_stats_file(&last_run_stat, &last_run_file, &f_stat, jsonArray);

		// Print the statistics
		print_stats(&last_run_print);

		// Send stats
		send_stats(jsonArray, &last_run_send);

		usleep(10000);
	}
}

static inline void
lcore_main_process(void)
{
	// initialization
	char *extractedName;
	uint16_t port;
	uint64_t timer_tsc = 0;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
	if (rte_eth_dev_socket_id(port) >= 0 &&
		rte_eth_dev_socket_id(port) !=
			(int)rte_socket_id())
		logMessage(__FILE__, __LINE__, "WARNING, port %u is on remote NUMA node to "
									   "polling thread.\n\tPerformance will "
									   "not be optimal.\n",
				   port);

	logMessage(__FILE__, __LINE__, "\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			   rte_lcore_id());

	// Main work of application loop
	while (!force_quit)
	{

		/* Get a burst of RX packets from the first port of the pair. */
		struct rte_mbuf *rx_bufs[BURST_SIZE];
		const uint16_t rx_count = rte_eth_rx_burst(0, 0, rx_bufs, BURST_SIZE);
		// uint64_t start_tsc = rte_rdtsc();
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
					logMessage(__FILE__, __LINE__, "Error copying packet to RST Client\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet                // Skip this packet
				}
				struct rte_mbuf *rst_pkt_server = rte_pktmbuf_copy(rx_pkt, rx_pkt->pool, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
				if (rst_pkt_server == NULL)
				{
					logMessage(__FILE__, __LINE__, "Error copying packet to RST Server\n");
					rte_pktmbuf_free(rx_pkt); // Free the original packet
				}

				// Apply modifications to the packets
				reset_tcp_client(rst_pkt_client);
				reset_tcp_server(rst_pkt_server);

				// Transmit modified packets
				const uint16_t rst_client_tx_count = rte_eth_tx_burst(1, 0, &rst_pkt_client, 1);
				if (rst_client_tx_count == 0)
				{
					logMessage(__FILE__, __LINE__, "Error sending packet to client\n");
					rte_pktmbuf_free(rst_pkt_client); // Free the modified packet
				}
				else
				{
					port_statistics[1].rstClient++;
				}

				const uint16_t rst_server_tx_count = rte_eth_tx_burst(1, 0, &rst_pkt_server, 1);
				if (rst_server_tx_count == 0)
				{
					logMessage(__FILE__, __LINE__, "Error sending packet to server\n");
					rte_pktmbuf_free(rst_pkt_server); // Free the modified packet
				}
				else
				{
					port_statistics[1].rstServer++;
				}
			}
			// uint64_t end_tsc = rte_rdtsc(); // Get end timestamp
			// uint64_t processing_time = end_tsc - start_tsc;
			// printf("Processing time: %" PRIu64 " cycles\n", processing_time);
			rte_pktmbuf_free(rx_pkt); // Free the original packet
		}
	}
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t real_size = size * nmemb;
	logMessage(__FILE__, __LINE__, "Heartbeat Response: %.*s \n", (int)real_size, (char *)contents);
	return real_size;
}

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

			logMessage(__FILE__, __LINE__, "Time : %s\n", timestamp_str);
			logMessage(__FILE__, __LINE__, "Post Fields : %s\n", post_fields);
			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

			res = curl_easy_perform(curl);

			if (res != CURLE_OK)
			{
				logMessage(__FILE__, __LINE__, "Heartbeat failed: %s\n", curl_easy_strerror(res));
			}
			sleep(5);
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
}

static inline void
lcore_sync_database()
{
	run_kafka_consumer();
}

int main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;
	unsigned lcore_id, lcore_main = 0, lcore_stats = 0, lcore_db = 0;
	init_database();

	// log the starting of the application
	logMessage(__FILE__, __LINE__, "Starting the application\n");

	// load the config file
	if (load_config_file())
	{
		logMessage(__FILE__, __LINE__, "Cannot load the config file\n");
		rte_exit(EXIT_FAILURE, "Cannot load the config file\n");
	}
	logMessage(__FILE__, __LINE__, "Load config done\n");

	// Initializion the Environment Abstraction Layer (EAL)
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
	{
		logMessage(__FILE__, __LINE__, "Error with EAL initialization\n");
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
	logMessage(__FILE__, __LINE__, "Clean the statistics data\n");

	// count the number of ports to send and receive
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
	{
		logMessage(__FILE__, __LINE__, "Error: number of ports must be even\n");
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");
	}

	// allocates the mempool to hold the mbufs
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
										MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	// check the mempool allocation
	if (mbuf_pool == NULL)
	{
		logMessage(__FILE__, __LINE__, "Cannot create mbuf pool\n");
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
	}
	logMessage(__FILE__, __LINE__, "Create mbuf pool done\n");

	// initializing ports
	RTE_ETH_FOREACH_DEV(portid)
	if (port_init(portid, mbuf_pool) != 0)
	{
		logMessage(__FILE__, __LINE__, "Cannot init port %" PRIu16 "\n", portid);
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);
	}

	// count the number of lcore
	if (rte_lcore_count() < 4)
	{
		logMessage(__FILE__, __LINE__, "lcore must be more than equal 4\n");
		rte_exit(EXIT_FAILURE, "lcore must be more than equal 4\n");
	}

	RTE_LCORE_FOREACH_WORKER(lcore_id)
	{
		if (lcore_id == (unsigned int)lcore_main ||
			lcore_id == (unsigned int)lcore_stats ||
			lcore_id == (unsigned int)lcore_db)
		{
			continue;
		}
		if (lcore_main == 0)
		{
			lcore_main = lcore_id;
			logMessage(__FILE__, __LINE__, "Main on core %u\n", lcore_id);
			continue;
		}
		if (lcore_stats == 0)
		{
			lcore_stats = lcore_id;
			logMessage(__FILE__, __LINE__, "Stats on core %u\n", lcore_id);
			continue;
		}
		if (lcore_db == 0)
		{
			lcore_db = lcore_id;
			logMessage(__FILE__, __LINE__, "DB on core %u\n", lcore_id);
			continue;
		}
	}

	// run the lcore main function
	logMessage(__FILE__, __LINE__, "Run the lcore main function\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_main_process,
						  NULL, lcore_main);

	// run the stats
	logMessage(__FILE__, __LINE__, "Run the stats\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_stats_process,
						  NULL, lcore_stats);

	logMessage(__FILE__, __LINE__, "Run the Kafka Broker\n");
	rte_eal_remote_launch((lcore_function_t *)lcore_sync_database,
						  NULL, lcore_db);

	// run the heartbeat
	logMessage(__FILE__, __LINE__, "Run the heartbeat\n");
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