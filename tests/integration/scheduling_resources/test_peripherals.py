"""
Scheduling and Resource Tests: Hardware Peripheral Resource Allocation (HAL)
Authoritative sources:
- Kube32 Hardware HAL Specification (PROJECT.md § 2, 4.2)
- survey_spec_miner_1/report.md § 5.1 (Peripheral Filter: esp.io/gpio, esp.io/i2c, esp.io/adc)
- survey_explorer_2/report.md § 1.2 (Capability-Based Peripheral Bitmask Firewall)
"""

from tests.integration.framework import BaseTestCase


class TestPeripherals(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.wait_for_node_memory()

    def test_gpio_request_within_capacity(self):
        """
        Verify a pod requesting 2 GPIO pins schedules onto a node with enough allocatable GPIOs.
        """
        pod = self.create_pod(self.unique_name("gpio-fit-pod"), {"memory": "32Ki", "esp32.io/gpio": "2"})
        self.assertTrue(len(pod["spec"]["nodeName"]) > 0)
        self.wait_for_phase(pod["metadata"]["name"], "Running")

    def test_excessive_gpio_request_remains_pending(self):
        """
        Verify a pod requesting one more GPIO pin than the node has fails the predicate
        filter and remains Pending.
        """
        gpios = int(self.first_node()["status"]["allocatable"]["esp32.io/gpio"])
        pod = self.create_pod(self.unique_name("gpio-excess-pod"), {"memory": "32Ki", "esp32.io/gpio": str(gpios + 1)})
        self.assertEqual(pod["status"]["phase"], "Pending")
        self.assertEqual(pod["spec"].get("nodeName", ""), "")

    def test_i2c_bus_request(self):
        """
        Verify a pod requesting the I2C peripheral bus binds to a node with I2C capacity.
        """
        pod = self.create_pod(self.unique_name("i2c-pod"), {"memory": "32Ki", "esp32.io/i2c": "1"})
        self.assertTrue(len(pod["spec"]["nodeName"]) > 0)

    def test_multi_peripheral_combination(self):
        """
        Verify a simultaneous multi-peripheral request (GPIO + I2C + memory) checks all predicates.
        """
        pod = self.create_pod(self.unique_name("multi-periph-pod"),
                              {"memory": "48Ki", "esp32.io/gpio": "4", "esp32.io/i2c": "1"})
        self.assertTrue(len(pod["spec"]["nodeName"]) > 0)

    def test_peripheral_deallocation_on_pod_delete(self):
        """
        Verify deleting a pod releases its peripherals back to the node: a pod holding every
        GPIO blocks the next one until it is deleted.
        """
        gpios = int(self.first_node()["status"]["allocatable"]["esp32.io/gpio"])
        p1 = self.create_pod(self.unique_name("gpio-fill"), {"memory": "16Ki", "esp32.io/gpio": str(gpios)})
        self.assertTrue(len(p1["spec"]["nodeName"]) > 0)

        # All GPIOs are booked: a pod asking for 5 must stay unbound
        p2 = self.create_pod(self.unique_name("gpio-blocked"), {"memory": "16Ki", "esp32.io/gpio": "5"})
        self.assertEqual(p2["spec"].get("nodeName", ""), "")

        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{p1['metadata']['name']}")
        self.assertEqual(del_resp.status_code, 200)

        # The scheduler retries pending pods once the capacity is back
        def bound():
            pod = self.http.get(f"/api/v1/namespaces/default/pods/{p2['metadata']['name']}").json()
            return pod["spec"].get("nodeName")
        self.wait_for_condition(bound, timeout=15.0, description="blocked pod to schedule after delete")
