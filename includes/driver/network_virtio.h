#ifndef ___NETWORK_VIRTIO_H
#define ___NETWORK_VIRTIO_H 0

#include <types.h>
#include <pci.h>
#include <driver/network_protocols.h>
#include <driver/virtio.h>
#include <linkedlist.h>

#define VIRTIO_NETWORK_F_CHEKCSUM         (1ULL <<  0)
#define VIRTIO_NETWORK_F_GUEST_CHEKCSUM   (1ULL <<  1)
#define VIRTIO_NETWORK_F_GUEST_OFFLOADS   (1ULL <<  2)
#define VIRTIO_NETWORK_F_MTU              (1ULL <<  3)
#define VIRTIO_NETWORK_F_MAC              (1ULL <<  5)
#define VIRTIO_NETWORK_F_GSO              (1ULL <<  6)
#define VIRTIO_NETWORK_F_GUEST_TSO4       (1ULL <<  7)
#define VIRTIO_NETWORK_F_GUEST_TSO6       (1ULL <<  8)
#define VIRTIO_NETWORK_F_GUEST_ECN        (1ULL <<  9)
#define VIRTIO_NETWORK_F_GUEST_UFO        (1ULL << 10)
#define VIRTIO_NETWORK_F_HOST_TSO4        (1ULL << 11)
#define VIRTIO_NETWORK_F_HOST_TSO6        (1ULL << 12)
#define VIRTIO_NETWORK_F_HOST_ECN         (1ULL << 13)
#define VIRTIO_NETWORK_F_HOST_UFO         (1ULL << 14)
#define VIRTIO_NETWORK_F_MRG_RXBUF        (1ULL << 15)
#define VIRTIO_NETWORK_F_STATUS           (1ULL << 16)
#define VIRTIO_NETWORK_F_CTRL_VQ          (1ULL << 17)
#define VIRTIO_NETWORK_F_CTRL_RX          (1ULL << 18)
#define VIRTIO_NETWORK_F_CTRL_VLAN        (1ULL << 19)
#define VIRTIO_NETWORK_F_GUEST_ANNOUNCE   (1ULL << 21)
#define VIRTIO_NETWORK_F_MQ               (1ULL << 22)
#define VIRTIO_NETWORK_F_CTRL_MAC_ADDR    (1ULL << 23)
#define VIRTIO_NETWORK_F_RSC_EXT          (1ULL << 61)
#define VIRTIO_NETWORK_F_STANDBY          (1ULL << 62)

#define VIRTIO_NETWORK_STATUS_LINK_UP     1
#define VIRTIO_NETWORK_STATUS_ANNOUNCE    2

typedef struct {
	mac_address_t mac;
	uint16_t status; /* if VIRTIO_NETWORK_F_STATUS */
	uint16_t max_virtual_queue_pairs; /* if VIRTIO_NETWORK_F_MQ */
	uint16_t mtu; /* if VIRTIO_NETWORK_F_MTU */
}__attribute__((packed)) virtio_network_config_t;

#define VIRTIO_NETWORK_HDR_F_NEEDS_CSUM    1
#define VIRTIO_NETWORK_HDR_F_DATA_VALID    2
#define VIRTIO_NETWORK_HDR_F_RSC_INFO      4

#define VIRTIO_NETWORK_HDR_GSO_UDP         3
#define VIRTIO_NETWORK_HDR_GSO_TCPV6       4
#define VIRTIO_NETWORK_HDR_GSO_ECN      0x80


typedef struct {
	uint8_t flags;
	uint8_t gso_type;
	uint16_t header_length;
	uint16_t gso_size;
	uint16_t checksum_start;
	uint16_t checksum_offset;
	uint16_t buffer_count;
}__attribute__((packed)) virtio_network_header_t;

#define VIRTIO_NETWORK_OK     0
#define VIRTIO_NETWORK_ERR    1

typedef struct {
	uint8_t class;
	uint8_t command;
	uint8_t command_spesific_data[];
	//uint8_t ack;
}__attribute__((packed)) virtio_network_control_t;

#define VIRTIO_NETWORK_CTRL_RX              0
#define VIRTIO_NETWORK_CTRL_RX_PROMISC      0
#define VIRTIO_NETWORK_CTRL_RX_ALLMULTI     1
#define VIRTIO_NETWORK_CTRL_RX_ALLUNI       2
#define VIRTIO_NETWORK_CTRL_RX_NOMULTI      3
#define VIRTIO_NETWORK_CTRL_RX_NOUNI        4
#define VIRTIO_NETWORK_CTRL_RX_NOBCAST      5

#define VIRTIO_NETWORK_CTRL_MAC    1
#define VIRTIO_NETWORK_CTRL_MAC_TABLE_SET        0
#define VIRTIO_NETWORK_CTRL_MAC_ADDR_SET         1

typedef struct {
	uint32_t entry_count;
	mac_address_t entries[];
}__attribute__((packed)) virtio_network_control_mac_t;

#define VIRTIO_NETWORK_CTRL_VLAN       2
#define VIRTIO_NETWORK_CTRL_VLAN_ADD             0
#define VIRTIO_NETWORK_CTRL_VLAN_DEL             1

#define VIRTIO_NET_CTRL_ANNOUNCE       3
#define VIRTIO_NET_CTRL_ANNOUNCE_ACK             0

struct  {
	uint16_t virtqueue_pairs;
} __attribute__((packed)) virtio_network_control_mq_t;

#define VIRTIO_NETWORK_CTRL_MQ    4
#define VIRTIO_NETWORK_CTRL_MQ_VQ_PAIRS_SET        0
#define VIRTIO_NETWORK_CTRL_MQ_VQ_PAIRS_MIN        1
#define VIRTIO_NETWORK_CTRL_MQ_VQ_PAIRS_MAX        0x8000


#define VIRTIO_NETWORK_CTRL_GUEST_OFFLOADS       5
#define VIRTIO_NETWORK_CTRL_GUEST_OFFLOADS_SET   0

#define VIRTIO_NETWORK_IOPORT_MAC             0x18
#define VIRTIO_NETWORK_IOPORT_LINK_STATUS     0x1E
#define VIRTIO_NETWORK_IOPORT_MAX_VQ_COUNT    0x20
#define VIRTIO_NETWORK_IOPORT_MTU             0x22

#define VIRTIO_NETWORK_QUEUE_ITEM_LENGTH        1536
#define VIRTIO_NETWORK_CTRL_QUEUE_ITEM_LENGTH     16

typedef struct {
	pci_dev_t* pci_netdev;
	boolean_t is_legacy;
	boolean_t has_msix;
	uint16_t msix_table_size;
	uint8_t msix_bir;
	uint32_t msix_table_offset;
	uint8_t msix_pendind_bit_bir;
	uint32_t msix_pendind_bit_table_offset;
	uint8_t irq_base;
	uint16_t iobase;
	virtio_pci_common_config_t* common_config;
	virtio_network_config_t* device_config;
	uint32_t notify_offset_multiplier;
	uint8_t* notify_offset;
	void* isr_config;
	mac_address_t mac;
	uint16_t max_vq_count;
	uint16_t mtu;
	uint64_t features;
	uint16_t queue_size;
	pci_capability_msix_t* msix_cap;
	virtio_queue_t* vq_rx;
	virtio_notification_data_t* nd_rx;
	virtio_queue_t* vq_tx;
	virtio_notification_data_t* nd_tx;
	virtio_queue_t* vq_ctrl;
	virtio_notification_data_t* nd_ctrl;
	linkedlist_t transmit_queue;
}network_virtio_dev_t;

int8_t network_virtio_init(pci_dev_t* pci_netdev);

#endif
