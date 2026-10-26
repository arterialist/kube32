package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/arterialist/kube32/federation/pkg/client"
	"github.com/arterialist/kube32/federation/pkg/ipc"
	"github.com/arterialist/kube32/federation/pkg/provider"
	"github.com/arterialist/kube32/federation/pkg/syncer"
)

func main() {
	var (
		kubeconfig      = flag.String("kubeconfig", "", "Path to upstream Kubernetes kubeconfig file")
		masterURL       = flag.String("master", "", "Upstream Kubernetes API server master URL")
		kube32API       = flag.String("kube32-api", "http://127.0.0.1:6443", "Downstream Kube32 Control Plane base URL")
		nodeName        = flag.String("node-name", "kube32-edge-gateway", "Virtual Node name registered in upstream Kubernetes")
		leaseInterval   = flag.Duration("lease-interval", 10*time.Second, "Heartbeat lease renewal interval")
		syncInterval    = flag.Duration("sync-interval", 2*time.Second, "Pod and capacity synchronization interval")
		channelIPCPort  = flag.Int("channel-ipc-port", 4242, "UDP port for K32C inter-pod virtual channel IPC mesh")
		gatewayHTTPPort = flag.Int("gateway-http-port", 8443, "HTTP REST gateway port for external channel IPC")
	)
	flag.Parse()

	log.Printf("[VIRTUAL-KUBELET] Initializing Kube32 Federation Bridge Daemon...")
	log.Printf("[VIRTUAL-KUBELET] Node Name: %s", *nodeName)
	log.Printf("[VIRTUAL-KUBELET] Downstream Kube32 API: %s", *kube32API)
	log.Printf("[VIRTUAL-KUBELET] Channel IPC UDP Port: %d | Gateway HTTP Port: %d", *channelIPCPort, *gatewayHTTPPort)

	// Build upstream Kubernetes client
	var restConfig *rest.Config
	var err error

	if *kubeconfig != "" || *masterURL != "" {
		restConfig, err = clientcmd.BuildConfigFromFlags(*masterURL, *kubeconfig)
	} else {
		// In-cluster config or default recommended kubeconfig
		restConfig, err = rest.InClusterConfig()
		if err != nil {
			homeKubeconfig := clientcmd.RecommendedHomeFile
			if _, statErr := os.Stat(homeKubeconfig); statErr == nil {
				restConfig, err = clientcmd.BuildConfigFromFlags("", homeKubeconfig)
			}
		}
	}

	if err != nil || restConfig == nil {
		log.Fatalf("[VIRTUAL-KUBELET] Failed to create upstream Kubernetes config: %v", err)
	}
	restConfig.ContentType = "application/json"

	kubeClient, err := kubernetes.NewForConfig(restConfig)
	if err != nil {
		log.Fatalf("[VIRTUAL-KUBELET] Failed to create upstream clientset: %v", err)
	}

	// Create Downstream Kube32 client
	k32Client := client.NewKube32Client(*kube32API)

	// Context with graceful shutdown signal
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	// 1. Start Low-Latency Channel IPC Gateway
	gateway := ipc.NewChannelGateway(*channelIPCPort, *gatewayHTTPPort)
	if err := gateway.Start(ctx); err != nil {
		log.Fatalf("[VIRTUAL-KUBELET] Failed to start Channel IPC Gateway: %v", err)
	}
	defer gateway.Stop()

	// 2. Start Node Registrar & Capacity Aggregator
	nodeCfg := &provider.NodeConfig{
		NodeName:          *nodeName,
		LeaseInterval:     *leaseInterval,
		SyncInterval:      *syncInterval,
		DisconnectTimeout: 15 * time.Second,
	}
	nodeCtrl := provider.NewNodeController(nodeCfg, kubeClient, k32Client)
	if err := nodeCtrl.Start(ctx); err != nil {
		log.Printf("[VIRTUAL-KUBELET] Warning starting NodeController: %v", err)
	}
	defer nodeCtrl.Stop()

	// 3. Start Bidirectional Pod Syncers
	syncerCfg := &syncer.SyncerConfig{
		VirtualNodeName: *nodeName,
		SyncInterval:    *syncInterval,
	}

	forwardSyncer := syncer.NewForwardSyncer(syncerCfg, kubeClient, k32Client)
	forwardSyncer.Start(ctx)
	defer forwardSyncer.Stop()

	reverseSyncer := syncer.NewReverseSyncer(syncerCfg, kubeClient, k32Client)
	reverseSyncer.Start(ctx)
	defer reverseSyncer.Stop()

	deletionSyncer := syncer.NewDeletionSyncer(syncerCfg, kubeClient, k32Client)
	deletionSyncer.Start(ctx)
	defer deletionSyncer.Stop()

	log.Printf("[VIRTUAL-KUBELET] Federation bridge is fully active. Awaiting pods and heartbeats...")

	// Block until signal received
	<-ctx.Done()
	log.Printf("[VIRTUAL-KUBELET] Termination signal received; shutting down gracefully...")
	time.Sleep(500 * time.Millisecond)
	fmt.Println("[VIRTUAL-KUBELET] Clean shutdown complete.")
}
