/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2013, Magnus Edenhill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Tests messages are produced in order.
 */


#include "test.h"

/* Typical include path would be <librdkafka/rdkafka.h>, but this program
 * is built from within the librdkafka source tree and thus differs. */
#include "rdkafka.h"  /* for Kafka driver */


static int msgid_next = 0;
static int msg_remains = 0;
static int fails = 0;

/**
 * Delivery reported callback.
 * Called for each message once to signal its delivery status.
 */
static void dr_single_partition_cb (rd_kafka_t *rk, void *payload, size_t len,
		   rd_kafka_resp_err_t err, void *opaque, void *msg_opaque) {
	int msgid = *(int *)msg_opaque;

	free(msg_opaque);

	if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
		TEST_FAIL("Message delivery failed: %s\n",
			  rd_kafka_err2str(err));

	if (msgid != msgid_next) {
		fails++;
		TEST_FAIL("Delivered msg %i, expected %i\n",
			 msgid, msgid_next);
		return;
	}

	msgid_next = msgid+1;
}

/* Produce a batch of messages to a single partition. */
static void test_single_partition (void) {
	int partition = 0;
	int r;
	rd_kafka_t *rk;
	rd_kafka_topic_t *rkt;
	rd_kafka_conf_t *conf;
	rd_kafka_topic_conf_t *topic_conf;
	char errstr[512];
	char msg[128];
	int msgcnt = 100000;
	int failcnt = 0;
	int i;
        rd_kafka_message_t *rkmessages;

        msgid_next = 0;

	test_conf_init(&conf, &topic_conf, 20);

	/* Set delivery report callback */
	rd_kafka_conf_set_dr_cb(conf, dr_single_partition_cb);

	/* Create kafka instance */
	rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf,
			  errstr, sizeof(errstr));
	if (!rk)
		TEST_FAIL("Failed to create rdkafka instance: %s\n", errstr);

	TEST_SAY("test_single_partition: Created kafka instance %s\n",
		 rd_kafka_name(rk));

	rkt = rd_kafka_topic_new(rk, test_mk_topic_name("0011", 0),
                                 topic_conf);
	if (!rkt)
		TEST_FAIL("Failed to create topic: %s\n",
			  rd_strerror(errno));

        /* Create messages */
        rkmessages = calloc(sizeof(*rkmessages), msgcnt);
	for (i = 0 ; i < msgcnt ; i++) {
		int *msgidp = malloc(sizeof(*msgidp));
		*msgidp = i;
		rd_snprintf(msg, sizeof(msg), "%s:%s test message #%i",
                         __FILE__, __FUNCTION__, i);

                rkmessages[i].payload  = rd_strdup(msg);
                rkmessages[i].len      = strlen(msg);
                rkmessages[i]._private = msgidp;
        }

        r = rd_kafka_produce_batch(rkt, partition, RD_KAFKA_MSG_F_FREE,
                                   rkmessages, msgcnt);

        /* Scan through messages to check for errors. */
        for (i = 0 ; i < msgcnt ; i++) {
                if (rkmessages[i].err) {
                        failcnt++;
                        if (failcnt < 100)
                                TEST_SAY("Message #%i failed: %s\n",
                                         i,
                                         rd_kafka_err2str(rkmessages[i].err));
                }
        }

        /* All messages should've been produced. */
        if (r < msgcnt) {
                TEST_SAY("Not all messages were accepted "
                         "by produce_batch(): %i < %i\n", r, msgcnt);
                if (msgcnt - r != failcnt)
                        TEST_SAY("Discrepency between failed messages (%i) "
                                 "and return value %i (%i - %i)\n",
                                 failcnt, msgcnt - r, msgcnt, r);
                TEST_FAIL("%i/%i messages failed\n", msgcnt - r, msgcnt);
        }

        free(rkmessages);
	TEST_SAY("Single partition: "
                 "Produced %i messages, waiting for deliveries\n", r);

	/* Wait for messages to be delivered */
	while (rd_kafka_outq_len(rk) > 0)
		rd_kafka_poll(rk, 50);

	if (fails)
		TEST_FAIL("%i failures, see previous errors", fails);

	if (msgid_next != msgcnt)
		TEST_FAIL("Still waiting for messages: next %i != end %i\n",
			  msgid_next, msgcnt);

	/* Destroy topic */
	rd_kafka_topic_destroy(rkt);

	/* Destroy rdkafka instance */
	TEST_SAY("Destroying kafka instance %s\n", rd_kafka_name(rk));
	rd_kafka_destroy(rk);

	return;
}



/**
 * Delivery reported callback.
 * Called for each message once to signal its delivery status.
 */
static void dr_partitioner_cb (rd_kafka_t *rk, void *payload, size_t len,
		   rd_kafka_resp_err_t err, void *opaque, void *msg_opaque) {
	int msgid = *(int *)msg_opaque;

	free(msg_opaque);

	if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
		TEST_FAIL("Message delivery failed: %s\n",
			  rd_kafka_err2str(err));

        if (msg_remains <= 0)
                TEST_FAIL("Too many message dr_cb callback calls "
                          "(at msgid #%i)\n", msgid);
        msg_remains--;
}

/* Produce a batch of messages using random (default) partitioner */
static void test_partitioner (void) {
	int partition = RD_KAFKA_PARTITION_UA;
	int r;
	rd_kafka_t *rk;
	rd_kafka_topic_t *rkt;
	rd_kafka_conf_t *conf;
	rd_kafka_topic_conf_t *topic_conf;
	char errstr[512];
	char msg[128];
	int msgcnt = 100000;
        int failcnt = 0;
	int i;
        rd_kafka_message_t *rkmessages;

        msg_remains = 0;

	test_conf_init(&conf, &topic_conf, 20);

	/* Set delivery report callback */
	rd_kafka_conf_set_dr_cb(conf, dr_partitioner_cb);

	/* Create kafka instance */
	rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf,
			  errstr, sizeof(errstr));
	if (!rk)
		TEST_FAIL("Failed to create rdkafka instance: %s\n", errstr);

	TEST_SAY("test_partitioner: Created kafka instance %s\n",
		 rd_kafka_name(rk));

	rkt = rd_kafka_topic_new(rk, test_mk_topic_name("0011", 0),
                                 topic_conf);
	if (!rkt)
		TEST_FAIL("Failed to create topic: %s\n",
			  rd_strerror(errno));

        /* Create messages */
        rkmessages = calloc(sizeof(*rkmessages), msgcnt);
	for (i = 0 ; i < msgcnt ; i++) {
		int *msgidp = malloc(sizeof(*msgidp));
		*msgidp = i;
		rd_snprintf(msg, sizeof(msg), "%s:%s test message #%i",
                         __FILE__, __FUNCTION__, i);

                rkmessages[i].payload = rd_strdup(msg);
                rkmessages[i].len     = strlen(msg);
                rkmessages[i]._private = msgidp;
        }

        msg_remains = msgcnt;
        r = rd_kafka_produce_batch(rkt, partition, RD_KAFKA_MSG_F_FREE,
                                   rkmessages, msgcnt);

        /* Scan through messages to check for errors. */
        for (i = 0 ; i < msgcnt ; i++) {
                if (rkmessages[i].err) {
                        failcnt++;
                        if (failcnt < 100)
                                TEST_SAY("Message #%i failed: %s\n",
                                         i,
                                         rd_kafka_err2str(rkmessages[i].err));
                }
        }

        /* All messages should've been produced. */
        if (r < msgcnt) {
                TEST_SAY("Not all messages were accepted "
                         "by produce_batch(): %i < %i\n", r, msgcnt);
                if (msgcnt - r != failcnt)
                        TEST_SAY("Discrepency between failed messages (%i) "
                                 "and return value %i (%i - %i)\n",
                                 failcnt, msgcnt - r, msgcnt, r);
                TEST_FAIL("%i/%i messages failed\n", msgcnt - r, msgcnt);
        }

        free(rkmessages);
	TEST_SAY("Partitioner: "
                 "Produced %i messages, waiting for deliveries\n", r);

	/* Wait for messages to be delivered */
	while (rd_kafka_outq_len(rk) > 0)
		rd_kafka_poll(rk, 50);

	if (fails)
		TEST_FAIL("%i failures, see previous errors", fails);

        if (msg_remains != 0)
		TEST_FAIL("Still waiting for %i/%i messages\n",
                          msg_remains, msgcnt);

	/* Destroy topic */
	rd_kafka_topic_destroy(rkt);

	/* Destroy rdkafka instance */
	TEST_SAY("Destroying kafka instance %s\n", rd_kafka_name(rk));
	rd_kafka_destroy(rk);

	return;
}

int main_0011_produce_batch (int argc, char **argv) {
        test_single_partition();
        test_partitioner();
	return 0;
}
