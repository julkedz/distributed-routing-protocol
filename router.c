#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/ip.h>
#include <poll.h>
#include <unistd.h>

#define PORT 54321
#define MAX_CONNECTIONS 100
#define ROUND_TIMEOUT_MS 3000

const uint32_t INF = UINT32_MAX;

struct Connection {
    struct in_addr network_bin;
    uint8_t mask_size;
    bool if_direct;
    uint32_t dist;
    struct in_addr via_ip;
    struct in_addr broadcast_bin;
    int age_counter;
    bool updated;
};

void ERROR(const char* str) {
    fprintf(stderr, "%s: %s\n", str, strerror(errno));
    exit(EXIT_FAILURE);
}

bool belongs_to_network(struct in_addr ip_addr, struct in_addr network, uint8_t mask_size) {
    uint32_t ip = ntohl(ip_addr.s_addr) & (0xFFFFFFFF << (32 - mask_size));
    return (ip == ntohl(network.s_addr));
}

int find_route_index(struct Connection* routing_table, int routing_table_idx, struct in_addr target_network) {
    for (int idx = 0; idx < routing_table_idx; idx++) {
        if (routing_table[idx].network_bin.s_addr == target_network.s_addr) {
            return idx;
        }
    }
    return -1;
}

// wczytanie interfejsów dla danego routera
void read_interfaces(
    struct Connection* directly, int* directly_idx,
    struct Connection* routing_table, int* routing_table_idx,
    struct in_addr* my_ips, int* n
) {
    scanf("%d", n);

    for (int i = 0; i < *n; i++) {
        struct Connection direct;
        char buffer[20];
        scanf("%s distance %u", buffer, &direct.dist);

        char* separator = "/";

        char ip[INET_ADDRSTRLEN];
        strcpy(ip, strtok(buffer, separator));
        direct.mask_size = atoi(strtok(NULL, separator));

        struct in_addr temp_addr;
        inet_pton(AF_INET, ip, &temp_addr);
        my_ips[i] = temp_addr;

        uint32_t ip_bin = ntohl(temp_addr.s_addr);
        ip_bin &= (0xFFFFFFFF << (32 - direct.mask_size));
        direct.network_bin.s_addr = htonl(ip_bin);

        ip_bin |= (0xFFFFFFFF >> (direct.mask_size));
        direct.broadcast_bin.s_addr = htonl(ip_bin);

        direct.if_direct = true;
        direct.via_ip.s_addr = INADDR_ANY;
        direct.age_counter = 0;
        direct.updated = true;

        directly[*directly_idx] = direct;
        (*directly_idx)++;
        routing_table[*routing_table_idx] = direct;
        (*routing_table_idx)++;
    }
}

int setup_socket(uint16_t port) {
    // tworzenie socketa
    int sockfd = socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );
    if (sockfd < 0) {
        close(sockfd);
        ERROR("socket error");
    }

    // ustawianie opcji gnizda żeby mógł nadawać na broadcast
    int so_value = 1;
    setsockopt(
        sockfd,
        SOL_SOCKET,
        SO_BROADCAST,
        (void*)&so_value,
        sizeof(so_value)
    );

    // wiązanie gniazda z adresem ip i portem
    struct sockaddr_in server_address = {0};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // nie ważne na jaki adres ip interfejsu przyjdzie wiadomość to i tak ją wyłapiemy

    int bind_result = bind (
        sockfd,
        (struct sockaddr*)&server_address,
        sizeof(server_address)
    );
    if (bind_result < 0) {
        close(sockfd);
        ERROR("bind error");
    }

    return sockfd;
}

ssize_t sending_packet(struct Connection* content, int sockfd, struct sockaddr_in* recipient) {
    uint8_t buffer[9];

    // ładowanie buffera
    memcpy(buffer, &(*content).network_bin, 4);
    memcpy(buffer + 4, &(*content).mask_size, 1);
    uint32_t net_dist = htonl((*content).dist);
    memcpy(buffer + 5, &net_dist, 4);

    ssize_t bytes_sent = sendto(
        sockfd,
        buffer,
        9,
        0,
        (struct sockaddr*)recipient,
        sizeof(*recipient)
    );

    return bytes_sent;
}

// rozsyłanie swojej tablicy przekazywania
void broadcast_routing_table(
    int sockfd,
    struct Connection* directly, int directly_idx,
    struct Connection* routing_table, int* routing_table_idx
) {
    struct sockaddr_in recipient = {0};
    recipient.sin_family = AF_INET;
    recipient.sin_port = htons(PORT);

    for (int target_idx = 0; target_idx < directly_idx; target_idx++) { // do kogo? do każdego kto jest w directly
        recipient.sin_addr = directly[target_idx].broadcast_bin;

        for (int content_idx = 0; content_idx < *routing_table_idx; content_idx++) { // a co? wszystko co mamy w routing_table
            ssize_t bytes_sent = sending_packet(&routing_table[content_idx], sockfd, &recipient);
            if (bytes_sent == -1) {
                for (int check_idx = 0; check_idx < *routing_table_idx; check_idx++) {
                    if (belongs_to_network(routing_table[check_idx].via_ip, directly[target_idx].network_bin, directly[target_idx].mask_size)) {
                        if (routing_table[check_idx].dist != INF) {
                            routing_table[check_idx].dist = INF;
                            routing_table[check_idx].updated = true;
                        }
                    }
                }
                break;
            }
            else { // jeśli udało się wysłać to znaczy, że sieć bezpośrednia jest osiągalna
                int found_idx = find_route_index(routing_table, *routing_table_idx, directly[target_idx].network_bin);
                if (found_idx == -1) {
                    routing_table[*routing_table_idx] = directly[target_idx];
                    (*routing_table_idx)++;
                }
                else if (routing_table[found_idx].dist >= directly[target_idx].dist) {
                    routing_table[found_idx] = directly[target_idx];
                }
            }
        }
    }
}

int find_direct_interface(struct Connection* directly, int directly_idx, struct in_addr ip) {
    for (int idx = 0; idx < directly_idx; idx++) {
        if (belongs_to_network(ip, directly[idx].network_bin, directly[idx].mask_size)) {
            return idx;
        }
    }
    return -1;
}

bool is_my_ip(struct in_addr ip, struct in_addr* my_ips, int n) {
    for (int idx = 0; idx < n; idx++) {
        if (ip.s_addr == my_ips[idx].s_addr) {
            return true;
        }
    }
    return false;
}

void handle_incoming_packet(
    const uint8_t* buffer,
    struct sockaddr_in* sender,
    struct Connection* directly, int directly_idx,
    struct Connection* routing_table, int* routing_table_idx,
    struct in_addr* my_ips, int n
) {
    // rozbieranie buffera
    struct in_addr recv_network_bin;
    uint8_t recv_mask_size;
    uint32_t recv_dist;

    memcpy(&recv_network_bin, buffer, 4);
    memcpy(&recv_mask_size, buffer + 4, 1);
    memcpy(&recv_dist, buffer + 5, 4);
    recv_dist = ntohl(recv_dist);

    // szukamy połączenia bezpośredniego z tym skąd nam przyszło
    int found_via = find_direct_interface(directly, directly_idx, sender->sin_addr);
    if (found_via == -1)
        return;

    // odrzucamy jeśli odebraliśmy pakiet od samych siebie
    if (is_my_ip(sender->sin_addr, my_ips, n))
        return;

    // aktualizacja bezpośredniego połączenia z senderem
    int found_via_in_routing_table = find_route_index(routing_table, *routing_table_idx, directly[found_via].network_bin);
    if (found_via_in_routing_table == -1) { // znaczy że trzeba dodać do routing_table
        routing_table[*routing_table_idx] = directly[found_via];
        (*routing_table_idx)++;
    }
    else if (routing_table[found_via_in_routing_table].dist >= directly[found_via].dist) {
        routing_table[found_via_in_routing_table] = directly[found_via];
    }

    // tworzenie obiektu Connection z tego co odczytaliśmy
    struct Connection con;
    con.network_bin = recv_network_bin;
    con.mask_size = recv_mask_size;
    con.if_direct = false;
    if (recv_dist > INF - directly[found_via].dist) // if (recv_dist + directly[found_via].dist > INF)
        con.dist = INF;
    else if (recv_dist + directly[found_via].dist > 10)
        con.dist = INF;
    else
        con.dist = recv_dist + directly[found_via].dist;
    con.via_ip = sender->sin_addr;
    con.broadcast_bin.s_addr = INADDR_ANY;
    con.age_counter = 0;
    con.updated = true;

    // szukamy wpisu w tablicy routingu z tym co nam przyszło
    int found_target = find_route_index(routing_table, *routing_table_idx, recv_network_bin);
    if (found_target == -1) { // to znaczy że nie ma pasującego wpisu w tablicy routing i musimy dodać nowy
        if (con.dist != INF) {
            routing_table[*routing_table_idx] = con;
            (*routing_table_idx)++;
        }
    }
    else { // jeśli istnieje odpowiadający wpis w tablicy routingu, być może będzie trzeba go poprawić
        if (routing_table[found_target].dist > con.dist) {
            routing_table[found_target] = con;
        }
        else if (routing_table[found_target].via_ip.s_addr == con.via_ip.s_addr) {
            if (routing_table[found_target].dist != INF || con.dist != INF)
                routing_table[found_target] = con;
        }
    }
}

// nasłuchiwanie przychodzących informacji
void receive_round_updates(
    int sockfd,
    struct Connection* directly, int directly_idx,
    struct Connection* routing_table, int* routing_table_idx,
    struct in_addr* my_ips, int n
) {

    int timeout = ROUND_TIMEOUT_MS;
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    uint8_t buffer[IP_MAXPACKET+1];

    // tworzenie obiektu do korzystania z poll()
    struct pollfd ps;
    ps.fd = sockfd;
    ps.events = POLLIN;
    ps.revents = 0;

    while (timeout > 0) {
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);
        int ready = poll(&ps, 1, timeout);
        gettimeofday(&end_time, NULL);
        int miliseconds = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_usec - start_time.tv_usec) / 1000;
        timeout -= miliseconds;

        if (ready == 0)
            break;
        else if (ready < 0) {
            close(sockfd);
            ERROR("poll error");
        }
        else {
            // pobieramy datagram UDP
            ssize_t datagram_len = recvfrom(
                sockfd,
                buffer,
                IP_MAXPACKET,
                0,
                (struct sockaddr*)&sender,
                &sender_len
            );
            if (datagram_len < 0) {
                close(sockfd);
                ERROR("recvfrom error");
            }

            handle_incoming_packet(
                buffer,
                &sender,
                directly, directly_idx,
                routing_table, routing_table_idx,
                my_ips, n
            );
        }
    }
}

// starzenie i usuwanie nieosiągalnych tras
void age_routing_table(struct Connection* routing_table, int* routing_table_idx) {
    for (int idx = 0; idx < *routing_table_idx; idx++) {
        if (routing_table[idx].updated == false) {
            routing_table[idx].age_counter++;
            if (routing_table[idx].age_counter >= 3) {
                if (routing_table[idx].dist < INF) {
                    routing_table[idx].dist = INF;
                    routing_table[idx].age_counter = 0;
                }
                else {
                    routing_table[idx] = routing_table[*routing_table_idx - 1];
                    (*routing_table_idx)--;
                    idx--;
                }
            }
        }
    }
}

void print_routing_table(struct Connection* routing_table, int routing_table_idx) {
    printf("----------------------------------\n");
    for (int idx=0; idx<routing_table_idx; idx++) {
        char network_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(routing_table[idx].network_bin.s_addr), network_ip_str, sizeof(network_ip_str));

        printf("%s/%u ", network_ip_str, routing_table[idx].mask_size);
        if (routing_table[idx].dist == INF)
            printf("unreachable ");
        else
            printf("distance %u ", routing_table[idx].dist);
        if (routing_table[idx].if_direct)
            printf("connected directly\n");
        else {
            char via_ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(routing_table[idx].via_ip.s_addr), via_ip_str, sizeof(via_ip_str));
            printf("via %s\n", via_ip_str);
        }
    }
}

int main() {
    struct Connection directly[MAX_CONNECTIONS], routing_table[MAX_CONNECTIONS];
    int directly_idx = 0, routing_table_idx = 0, n = 0;
    struct in_addr my_ips[MAX_CONNECTIONS];

    // wczytanie interfejsów dla danego routera
    read_interfaces(directly, &directly_idx, routing_table, &routing_table_idx, my_ips, &n);

    // tworzenie i konfiguracja gniazda
    int sockfd = setup_socket(PORT);

    while (1) {
        // oznaczamy wszystkie połączenia jako niezaktualizowane w tej rundzie
        for (int  idx=0; idx<routing_table_idx; idx++) {
            routing_table[idx].updated = false;
        }

        // rozsyłanie swojej tablicy przekazywania
        broadcast_routing_table(sockfd, directly, directly_idx, routing_table, &routing_table_idx);

        // nasłuchiwanie przychodzących informacji
        receive_round_updates(sockfd, directly, directly_idx, routing_table, &routing_table_idx, my_ips, n);

        // starzenie i usuwanie nieosiągalnych tras
        age_routing_table(routing_table, &routing_table_idx);

        // wypisujemy routing_table raz na rundę
        print_routing_table(routing_table, routing_table_idx);
    }

    close(sockfd);
    return 0;
}
