/* Real broker proof: stalled exact reader, ordered fanout, independent traffic,
 * bounded recovery, and a publisher that closes after its final write. */
typedef struct {
    vbus_client *client;
    atomic_int count;
    atomic_int invalid;
    atomic_int stop;
} flow_reader_probe;

typedef struct {
    vbus_client *client;
    atomic_int sent;
    atomic_int done;
    int result;
} flow_writer_probe;

enum { FLOW_TEST_MESSAGES = 1536, FLOW_TEST_PAYLOAD = 4096 };

static void flow_read(const char *topic, const char *reply, const uint8_t *body,
    size_t length, void *user) {
    flow_reader_probe *probe = user;
    uint32_t sequence = 0;
    int count = atomic_load_explicit(&probe->count, memory_order_relaxed);
    if (!topic || strcmp(topic, "test.flow") != 0 || reply || length != FLOW_TEST_PAYLOAD) {
        atomic_store_explicit(&probe->invalid, 1, memory_order_relaxed);
    } else {
        memcpy(&sequence, body, sizeof(sequence));
        if (sequence != (uint32_t)count)
            atomic_store_explicit(&probe->invalid, 1, memory_order_relaxed);
        for (size_t i = sizeof(sequence); i < length; ++i) {
            if (body[i] != (uint8_t)sequence) {
                atomic_store_explicit(&probe->invalid, 1, memory_order_relaxed);
                break;
            }
        }
    }
    atomic_store_explicit(&probe->count, count + 1, memory_order_release);
}

static void *flow_read_thread(void *user) {
    flow_reader_probe *probe = user;
    while (!atomic_load_explicit(&probe->stop, memory_order_relaxed)) {
        if (vbus_poll(probe->client, 10) != 0) {
            atomic_store_explicit(&probe->invalid, 1, memory_order_relaxed);
            break;
        }
    }
    return NULL;
}

static void *flow_write_thread(void *user) {
    flow_writer_probe *probe = user;
    uint8_t body[FLOW_TEST_PAYLOAD];
    for (uint32_t i = 0u; i < FLOW_TEST_MESSAGES; ++i) {
        memset(body, (uint8_t)i, sizeof(body));
        memcpy(body, &i, sizeof(i));
        if (vbus_publish(probe->client, "test.flow", body, sizeof(body)) != 0) {
            probe->result = -1;
            break;
        }
        atomic_store_explicit(&probe->sent, (int)i + 1, memory_order_release);
    }
    /* HUP must not discard a publication held for the recovering reader. */
    vbus_close(probe->client);
    probe->client = NULL;
    atomic_store_explicit(&probe->done, 1, memory_order_release);
    return NULL;
}

static int flow_control_case(int recover) {
    path_broker_probe broker = {0};
    flow_reader_probe slow = {0}, observer = {0};
    flow_writer_probe writer = {0};
    vbus_client *marker_pub = NULL, *marker_sub = NULL;
    pthread_t broker_worker, reader_worker, writer_worker;
    int broker_started = 0, reader_started = 0, writer_started = 0;
    int marker = 0, ok = 0, error = 0, rc = 0, waiting = 0, previous_sent = -1;
    struct timespec pause = {0, 10000000L};
    struct pollfd producer = {.fd = -1, .events = POLLOUT};
    snprintf(broker.path, sizeof(broker.path), "/tmp/vbus-flow-%d-%d.sock", (int)getpid(), recover);
    setenv("VBUS_BROKER_MAX_BUFFER_BYTES", "16777216", 1);
    if (pthread_create(&broker_worker, NULL, path_broker_thread, &broker) != 0) goto done;
    broker_started = 1;
    for (int i = 0; i < 100 && !writer.client; ++i) {
        writer.client = vbus_connect(broker.path);
        if (!writer.client) usleep(10000);
    }
    slow.client = vbus_connect_bounded(broker.path, 8192u);
    observer.client = vbus_connect(broker.path);
    marker_pub = vbus_connect(broker.path);
    marker_sub = vbus_connect(broker.path);
    if (!writer.client || !slow.client || !observer.client || !marker_pub || !marker_sub) goto done;
    /* Bound kernel queues too. Host defaults can absorb the entire fixture
     * without filling the broker's four-MiB flow window. */
    int socket_bytes = 8192;
    if (setsockopt(vbus_poll_fd(slow.client), SOL_SOCKET, SO_RCVBUF,
            &socket_bytes, sizeof(socket_bytes)) != 0 ||
        setsockopt(vbus_poll_fd(writer.client), SOL_SOCKET, SO_SNDBUF,
            &socket_bytes, sizeof(socket_bytes)) != 0) goto done;
    if (vbus_subscribe_flow(slow.client, "test.flow", flow_read, &slow) != 0 ||
        vbus_subscribe(observer.client, "test.flow", NULL, flow_read, &observer) != 0 ||
        vbus_subscribe(marker_sub, "test.flow-marker", NULL, on_count, &marker) != 0) goto done;
    if (pthread_create(&reader_worker, NULL, flow_read_thread, &observer) != 0) goto done;
    reader_started = 1;
    producer.fd = vbus_poll_fd(writer.client);
    if (pthread_create(&writer_worker, NULL, flow_write_thread, &writer) != 0) goto done;
    writer_started = 1;
    /* Start the stall clock after the producer reaches backpressure. A fixed
     * startup delay can expire before instrumented runs fill the queue. */
    for (int i = 0; i < 300 && waiting < 3; ++i) {
        if (atomic_load_explicit(&writer.done, memory_order_acquire)) goto done;
        int ready = poll(&producer, 1, 0);
        if (ready < 0 || (producer.revents & (POLLERR | POLLHUP | POLLNVAL))) goto done;
        int sent = atomic_load_explicit(&writer.sent, memory_order_acquire);
        /* Headers and host socket queues change the publication count needed
         * to fill the flow window. Observe blocked progress, not a fixed count. */
        if (sent > 0 && (ready == 0 || sent == previous_sent))
            waiting++;
        else
            waiting = 0;
        previous_sent = sent;
        nanosleep(&pause, NULL);
    }
    if (waiting != 3) goto done;
    /* This independent publisher must progress while the large one waits. */
    if (vbus_publish(marker_pub, "test.flow-marker", (const uint8_t *)"ok", 2u) != 0) goto done;
    for (int i = 0; i < 20 && !marker; ++i) {
        if (vbus_poll(marker_sub, 10) != 0) goto done;
    }
    if (marker != 1 || atomic_load_explicit(&slow.count, memory_order_acquire) != 0) goto done;
    if (!recover) {
        pause.tv_sec = 1;
        pause.tv_nsec = 400000000L;
        nanosleep(&pause, NULL);
    }
    for (int i = 0; i < 5000 &&
        atomic_load_explicit(&slow.count, memory_order_acquire) < FLOW_TEST_MESSAGES; ++i) {
        int previous = atomic_load_explicit(&slow.count, memory_order_acquire);
        rc = vbus_poll_one(slow.client, 2);
        int current = atomic_load_explicit(&slow.count, memory_order_acquire);
        if (current > previous + 1 || (rc == 0 && current != previous)) error = 1;
        if (rc < 0) break;
    }
    if (recover ? rc < 0 || atomic_load_explicit(&slow.count, memory_order_acquire) != FLOW_TEST_MESSAGES : rc >= 0)
        goto done;
    for (int i = 0; i < 200 &&
        (!atomic_load_explicit(&writer.done, memory_order_acquire) ||
         atomic_load_explicit(&observer.count, memory_order_acquire) != FLOW_TEST_MESSAGES); ++i)
        usleep(10000);
    ok = !error && !atomic_load_explicit(&slow.invalid, memory_order_relaxed) &&
        !atomic_load_explicit(&observer.invalid, memory_order_relaxed) &&
        atomic_load_explicit(&writer.done, memory_order_acquire) && writer.result == 0 &&
        atomic_load_explicit(&writer.sent, memory_order_acquire) == FLOW_TEST_MESSAGES &&
        atomic_load_explicit(&observer.count, memory_order_acquire) == FLOW_TEST_MESSAGES;
done:
    if (!ok) fprintf(stderr,
        "flow case recover=%d waiting=%d sent=%d done=%d slow=%d observer=%d invalid=%d/%d poll=%d marker=%d\n",
        recover, waiting, atomic_load_explicit(&writer.sent, memory_order_acquire),
        atomic_load_explicit(&writer.done, memory_order_acquire),
        atomic_load_explicit(&slow.count, memory_order_acquire),
        atomic_load_explicit(&observer.count, memory_order_acquire),
        atomic_load_explicit(&slow.invalid, memory_order_relaxed),
        atomic_load_explicit(&observer.invalid, memory_order_relaxed), rc, marker);
    vbus_close(slow.client);
    if (reader_started) {
        atomic_store_explicit(&observer.stop, 1, memory_order_relaxed);
        pthread_join(reader_worker, NULL);
    }
    vbus_close(observer.client);
    vbus_close(marker_pub);
    vbus_close(marker_sub);
    if (broker_started) {
        atomic_store_explicit(&broker.stop, 1, memory_order_relaxed);
        pthread_join(broker_worker, NULL);
    }
    if (writer_started) pthread_join(writer_worker, NULL);
    else vbus_close(writer.client);
    unlink(broker.path);
    return ok ? 0 : -1;
}

static int bounded_reader_case(const char *path, vbus_client *publisher) {
    vbus_client *reader = vbus_connect_bounded(path, 8192u);
    uint8_t body[8192] = {0};
    int count = 0, rc = 0, ok = 0;
    if (!reader) return -1;
    if (vbus_subscribe_flow(reader, "test.bad.>", on_count, &count) != -1 ||
        vbus_subscribe_flow(reader, "test.bad.*", on_count, &count) != -1 ||
        vbus_subscribe_flow(reader, "test.receive-bound", on_count, &count) != 0 ||
        vbus_subscribe_flow(reader, "test.second", on_count, &count) != -1) goto done;
    /* Body alone fits the limit; the complete VBus frame does not. */
    if (vbus_publish(publisher, "test.receive-bound", body, sizeof(body)) != 0) goto done;
    for (int i = 0; i < 100 && rc >= 0; ++i) rc = vbus_poll_one(reader, 10);
    ok = rc == -1 && count == 0;
done:
    vbus_close(reader);
    return ok ? 0 : -1;
}

static int raw_flow_rejected(const char *path, int variant) {
    vbus_client *client = vbus_connect(path);
    raw_vbus_header header = {0};
    uint8_t frame[128];
    const char *topic = variant == 0 ? "test.*" : variant == 1 ? "test.>" : "test.flow";
    size_t offset = sizeof(header);
    int count = 0, result = -1;
    if (!client) return -1;
    if (variant == 5 && vbus_subscribe(client, "test.first", NULL, on_count, &count) != 0)
        goto done;
    if (variant >= 6 && vbus_subscribe_flow(client, "test.flow", on_count, &count) != 0)
        goto done;
    header.magic = VBUS_MAGIC;
    header.version = VBUS_VERSION;
    header.op = variant == 6 ? VBUS_OP_PUB : variant == 7 ? VBUS_OP_SUB : VBUS_OP_SUB_FLOW;
    header.subscription_id = variant == 6 ? VBUS_SUBSCRIPTION_NONE : variant >= 5 ? 1u : 0u;
    header.topic_len = (uint32_t)strlen(topic);
    header.queue_len = variant == 2 ? 1u : 0u;
    header.reply_len = variant == 3 ? 1u : 0u;
    header.body_len = variant == 4 || variant == 6 ? 1u : 0u;
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + offset, topic, header.topic_len);
    offset += header.topic_len;
    memset(frame + offset, 'x', header.queue_len + header.reply_len + header.body_len);
    offset += header.queue_len + header.reply_len + header.body_len;
    /* Bypass the client API to test the broker's wire contract itself. */
    if (write_all(vbus_poll_fd(client), frame, offset) == 0)
        result = wait_for_peer_close(vbus_poll_fd(client), 1000);
done:
    vbus_close(client);
    return result;
}
