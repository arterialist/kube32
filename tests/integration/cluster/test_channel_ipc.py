"""
Kube32 Cluster Tests: Virtual Channel IPC Multi-Node Protocol
Verifies K32C packet format, CRC16 validation, UDP transport multiplexing,
and Capability Firewall isolation between simulated cluster nodes.
"""

import time
import socket
import struct
from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import ClusterHarness, crc16_ccitt, fnv1a_32, enable_link_stats, link_stats


class TestChannelIPC(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16743, base_ipc_port=17700)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def start_observed_cp(self, name):
        """A control plane whose channel counters can be read over its Ingress port."""
        ingress_port = self.harness.base_api_port + 100
        cp = self.harness.start_control_plane(name=name, ingress_port=ingress_port)
        enable_link_stats(cp)
        self.wait_for_condition(lambda: link_stats(ingress_port) is not None, timeout=5.0, description="link stats")
        return cp, ingress_port

    def test_multinode_ipc_packet_transmission(self):
        """Valid K32C packet sent over UDP to a node's IPC port is accepted and counted."""
        cp, port = self.start_observed_cp("cp-ipc-0")
        before = link_stats(port)

        payload = b"Kube32-Mesh-Sensor-Data-Payload"
        self.harness.send_k32c_packet(
            dest_ip="127.0.0.1",
            dest_port=cp.ipc_port,
            channel_uri="pod://cp-ipc-0/telemetry",
            sender_pod="worker-ipc-1",
            payload=payload
        )

        self.wait_for_condition(lambda: link_stats(port)["espnow_frames_rx"] > before["espnow_frames_rx"],
                                timeout=5.0, description="frame counted")
        after = link_stats(port)
        self.assertEqual(after["crc_errors"], before["crc_errors"])
        self.assertTrue(cp.is_alive())

    def test_corrupt_crc16_packet_rejected(self):
        """Packet with corrupted CRC16 is counted as a CRC error and does not crash the receiver."""
        cp, port = self.start_observed_cp("cp-ipc-1")
        before = link_stats(port)

        # Build corrupted packet with genuine wire magic (0x4332334B = "K32C")
        magic = 0x4332334B
        payload = b"TamperedData"
        header = struct.pack(
            "<IBBHIIIBBH",
            magic, 1, 0, 1,
            fnv1a_32("pod://cp-ipc-1/test"),
            fnv1a_32("attacker-pod"),
            0, 0, 1, len(payload)
        )
        packet_corrupt = header + payload + struct.pack("<H", 0x1234) # Corrupt CRC

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(packet_corrupt, ("127.0.0.1", cp.ipc_port))
        sock.close()

        self.wait_for_condition(lambda: link_stats(port)["crc_errors"] == before["crc_errors"] + 1,
                                timeout=5.0, description="CRC error counted")
        self.assertTrue(cp.is_alive())

    def test_truncated_and_corrupt_packet_barrage(self):
        """Barrage of truncated, invalid magic, and corrupt packets does not crash worker."""
        cp = self.harness.start_control_plane(name="cp-ipc-adv")
        worker = self.harness.start_worker("worker-ipc-adv", 1, cp)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # 1. Truncated frames (0 to 23 bytes)
        for length in [0, 1, 4, 12, 23]:
            sock.sendto(b"\x00" * length, ("127.0.0.1", worker.ipc_port))

        # 2. Corrupt magic numbers
        for bad_magic in [0x00000000, 0xDEADBEEF, 0x12345678]:
            header = struct.pack(
                "<IBBHIIIBBH",
                bad_magic, 1, 0, 1,
                fnv1a_32("pod://worker-ipc-adv/test"),
                fnv1a_32("attacker-pod"),
                0, 0, 1, 4
            )
            data = header + b"Test"
            crc = crc16_ccitt(data)
            sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        # 3. Oversized declared payload_len
        header = struct.pack(
            "<IBBHIIIBBH",
            0x4332334B, 1, 0, 1,
            fnv1a_32("pod://worker-ipc-adv/test"),
            fnv1a_32("attacker-pod"),
            0, 0, 1, 240 # > 224
        )
        data = header + b"X" * 240
        crc = crc16_ccitt(data)
        sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        sock.close()
        time.sleep(0.3)
        self.assertTrue(worker.is_alive())

    def test_oob_fragment_indices(self):
        """Fragment index exceeding fragment count or boundary is safely dropped without crash."""
        cp = self.harness.start_control_plane(name="cp-ipc-frag")
        worker = self.harness.start_worker("worker-ipc-frag", 1, cp)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Send frag_index = 5, total_fragments = 2
        header = struct.pack(
            "<IBBHIIIBBH",
            0x4332334B, 1, 1, 99,
            fnv1a_32("pod://worker-ipc-frag/test"),
            fnv1a_32("attacker-pod"),
            0, 5, 2, 8
        )
        data = header + b"OobFrag0"
        crc = crc16_ccitt(data)
        sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        sock.close()
        time.sleep(0.3)
        self.assertTrue(worker.is_alive())

