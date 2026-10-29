"""
API Resource Tests: Server-Side Table Serialization
Authoritative sources:
- Kubernetes meta.k8s.io/v1 Table Schema (io.k8s.apimachinery.pkg.apis.meta.v1.Table)
- survey_spec_miner_1/report.md § 2.1, 2.2, 2.3
- PROJECT.md § 2 (Feature 6: Server-Side Table Serializer)
"""

from tests.integration.framework import BaseTestCase

TABLE_ACCEPT_HEADER = {
    "Accept": "application/json;as=Table;v=v1;g=meta.k8s.io, application/json;q=0.9"
}


class TestTables(BaseTestCase):
    def test_node_table_serialization(self):
        """
        Verify GET /api/v1/nodes with Accept: as=Table returns kind Table.
        """
        resp = self.http.get("/api/v1/nodes", headers=TABLE_ACCEPT_HEADER)
        self.assertEqual(resp.status_code, 200)
        table = resp.json()
        self.assertEqual(table.get("kind"), "Table")
        self.assertEqual(table.get("apiVersion"), "meta.k8s.io/v1")
        self.assertIn("columnDefinitions", table)
        self.assertIn("rows", table)

    def test_node_table_columns_and_priorities(self):
        """
        Verify Node Table contains Priority 0 (standard) and Priority 1 (wide) column definitions.
        """
        resp = self.http.get("/api/v1/nodes", headers=TABLE_ACCEPT_HEADER)
        table = resp.json()
        cols = table.get("columnDefinitions", [])
        col_names = [c.get("name") for c in cols]

        # Priority 0 columns (standard kubectl get nodes)
        self.assertIn("NAME", col_names)
        self.assertIn("STATUS", col_names)
        self.assertIn("ROLES", col_names)
        self.assertIn("AGE", col_names)
        self.assertIn("VERSION", col_names)

        # Priority 1 columns (-o wide)
        self.assertIn("INTERNAL-IP", col_names)
        self.assertIn("OS-IMAGE", col_names)

        # Verify priority levels
        name_col = next(c for c in cols if c.get("name") == "NAME")
        self.assertEqual(name_col.get("priority"), 0)

        ip_col = next(c for c in cols if c.get("name") == "INTERNAL-IP")
        self.assertEqual(ip_col.get("priority"), 1)

    def test_pod_table_serialization(self):
        """
        Verify GET /api/v1/namespaces/{ns}/pods with Accept: as=Table returns kind Table
        with Pod column definitions: NAME, READY, STATUS, RESTARTS, AGE.
        """
        pod_name = self.unique_name("tbl-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://embedded/echo"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        resp = self.http.get("/api/v1/namespaces/default/pods", headers=TABLE_ACCEPT_HEADER)
        self.assertEqual(resp.status_code, 200)
        table = resp.json()
        self.assertEqual(table.get("kind"), "Table")
        cols = [c.get("name") for c in table.get("columnDefinitions", [])]
        self.assertIn("NAME", cols)
        self.assertIn("READY", cols)
        self.assertIn("STATUS", cols)
        self.assertIn("RESTARTS", cols)
        self.assertIn("AGE", cols)

        # Check rows contain our pod
        rows = table.get("rows", [])
        row_names = [r["cells"][0] for r in rows if r.get("cells")]
        self.assertIn(pod_name, row_names)

    def test_deployment_table_serialization(self):
        """
        Verify Deployment table serialization returns NAME, READY, UP-TO-DATE, AVAILABLE, AGE.
        """
        deploy_name = self.unique_name("tbl-deploy")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        resp = self.http.get("/apis/apps/v1/namespaces/default/deployments", headers=TABLE_ACCEPT_HEADER)
        self.assertEqual(resp.status_code, 200)
        table = resp.json()
        self.assertEqual(table.get("kind"), "Table")
        cols = [c.get("name") for c in table.get("columnDefinitions", [])]
        self.assertIn("NAME", cols)
        self.assertIn("READY", cols)
        self.assertIn("UP-TO-DATE", cols)
        self.assertIn("AVAILABLE", cols)

    def test_kubectl_get_nodes_table_cli(self):
        """
        Verify kubectl get nodes formats stdout with standard column headers.
        """
        res = self.kubectl.run(["get", "nodes"])
        self.assertTrue(res.ok, f"kubectl get nodes failed: {res.stderr}")
        header_line = res.stdout.strip().split("\n")[0]
        self.assertIn("NAME", header_line)
        self.assertIn("STATUS", header_line)
        self.assertIn("ROLES", header_line)
        self.assertIn("VERSION", header_line)

    def test_kubectl_get_nodes_wide_table_cli(self):
        """
        Verify kubectl get nodes -o wide includes Priority 1 columns in CLI output.
        """
        res = self.kubectl.run(["get", "nodes", "-o", "wide"])
        self.assertTrue(res.ok, f"kubectl get nodes -o wide failed: {res.stderr}")
        header_line = res.stdout.strip().split("\n")[0]
        self.assertIn("INTERNAL-IP", header_line)
        self.assertIn("OS-IMAGE", header_line)
