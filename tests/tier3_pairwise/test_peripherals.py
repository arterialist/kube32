"""
Tier 3 Pairwise Tests: Hardware Peripheral Resource Allocation (HAL)
Authoritative sources:
- Kube32 Hardware HAL Specification (PROJECT.md § 2, 4.2)
- survey_spec_miner_1/report.md § 5.1 (Peripheral Filter: esp.io/gpio, esp.io/i2c, esp.io/adc)
- survey_explorer_2/report.md § 1.2 (Capability-Based Peripheral Bitmask Firewall)
"""

from tests.framework import BaseTestCase


class TestPeripherals(BaseTestCase):
    def test_gpio_request_within_capacity(self):
        """
        Verify pod requesting 2 GPIO pins schedules successfully to a node with 14 allocatable GPIOs.
        """
        pod_name = self.unique_name("gpio-fit-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "gpio-worker",
                        "image": "wasm://gpio-toggle",
                        "resources": {
                            "requests": {
                                "memory": "32Ki",
                                "esp.io/gpio": "2"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()
        self.assertEqual(pod["status"]["phase"], "Running")
        self.assertTrue(len(pod["spec"]["nodeName"]) > 0)

    def test_excessive_gpio_request_remains_pending(self):
        """
        Verify pod requesting 30 GPIO pins (exceeding maximum 14-16 allocatable GPIOs)
        fails predicate filter and remains in Pending state.
        """
        pod_name = self.unique_name("gpio-excess-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "huge-gpio-worker",
                        "image": "wasm://gpio-toggle",
                        "resources": {
                            "requests": {
                                "memory": "32Ki",
                                "esp.io/gpio": "30"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()
        self.assertEqual(pod["status"]["phase"], "Pending")
        self.assertEqual(pod["spec"]["nodeName"], "")

    def test_i2c_bus_request(self):
        """
        Verify pod requesting I2C peripheral bus binds to node with I2C capacity.
        """
        pod_name = self.unique_name("i2c-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "i2c-sensor",
                        "image": "wasm://bmp280",
                        "resources": {
                            "requests": {
                                "memory": "32Ki",
                                "esp.io/i2c": "1"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        self.assertEqual(resp.json()["status"]["phase"], "Running")

    def test_multi_peripheral_combination(self):
        """
        Verify simultaneous multi-peripheral request (GPIO + I2C + Memory) checks all predicates.
        """
        pod_name = self.unique_name("multi-periph-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "complex-device",
                        "image": "wasm://robot",
                        "resources": {
                            "requests": {
                                "memory": "48Ki",
                                "esp.io/gpio": "4",
                                "esp.io/i2c": "1"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        self.assertEqual(resp.json()["status"]["phase"], "Running")

    def test_peripheral_deallocation_on_pod_delete(self):
        """
        Verify deleting a pod releases its allocated peripherals back to the cluster pool.
        """
        # Exhaust GPIO on node 1 and node 2 (14 GPIO each = 28 total)
        # Pod 1: 14 GPIO -> Node 1
        # Pod 2: 14 GPIO -> Node 2
        # Pod 3: 5 GPIO -> Pending!
        # Delete Pod 1 -> Pod 4 with 5 GPIO can now schedule!
        p1 = self.unique_name("gpio-fill-1")
        p2 = self.unique_name("gpio-fill-2")
        self.register_cleanup("pod", "default", p1)
        self.register_cleanup("pod", "default", p2)

        def make_pod(name, gpios):
            return {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": name, "namespace": "default"},
                "spec": {
                    "containers": [{
                        "name": "w", "image": "wasm://app",
                        "resources": {"requests": {"memory": "16Ki", "esp.io/gpio": str(gpios)}}
                    }]
                }
            }

        resp1 = self.http.post("/api/v1/namespaces/default/pods", body=make_pod(p1, 14))
        self.assertEqual(resp1.json()["status"]["phase"], "Running")

        resp2 = self.http.post("/api/v1/namespaces/default/pods", body=make_pod(p2, 14))
        self.assertEqual(resp2.json()["status"]["phase"], "Running")

        # Now all GPIOs are booked. Pod 3 requesting 5 GPIO must be Pending
        p3 = self.unique_name("gpio-blocked")
        self.register_cleanup("pod", "default", p3)
        resp3 = self.http.post("/api/v1/namespaces/default/pods", body=make_pod(p3, 5))
        self.assertEqual(resp3.json()["status"]["phase"], "Pending")

        # Delete Pod 1
        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{p1}")
        self.assertEqual(del_resp.status_code, 200)

        # Now Pod 4 requesting 5 GPIO should successfully schedule
        p4 = self.unique_name("gpio-succeed-after-del")
        self.register_cleanup("pod", "default", p4)
        resp4 = self.http.post("/api/v1/namespaces/default/pods", body=make_pod(p4, 5))
        self.assertEqual(resp4.json()["status"]["phase"], "Running")
