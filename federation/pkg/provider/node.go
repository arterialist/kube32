package provider

import (
	"context"
	"fmt"
	"log"
	"strconv"
	"strings"
	"sync"
	"time"

	coordinationv1 "k8s.io/api/coordination/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"

	"github.com/arterialist/kube32/federation/pkg/client"
)

type NodeConfig struct {
	NodeName          string
	LeaseInterval     time.Duration
	SyncInterval      time.Duration
	DisconnectTimeout time.Duration
}

type NodeController struct {
	cfg        *NodeConfig
	kubeClient kubernetes.Interface
	k32Client  *client.Kube32Client

	lastK32Success time.Time
	mu             sync.RWMutex
	stopCh         chan struct{}
}

func NewNodeController(cfg *NodeConfig, kubeClient kubernetes.Interface, k32Client *client.Kube32Client) *NodeController {
	if cfg.LeaseInterval <= 0 {
		cfg.LeaseInterval = 10 * time.Second
	}
	if cfg.SyncInterval <= 0 {
		cfg.SyncInterval = 2 * time.Second
	}
	if cfg.DisconnectTimeout <= 0 {
		cfg.DisconnectTimeout = 15 * time.Second
	}
	return &NodeController{
		cfg:            cfg,
		kubeClient:     kubeClient,
		k32Client:      k32Client,
		lastK32Success: time.Now(),
		stopCh:         make(chan struct{}),
	}
}

func (c *NodeController) Start(ctx context.Context) error {
	// 1. Initial Node Registration
	if err := c.reconcileNode(ctx); err != nil {
		log.Printf("[NODE-CTRL] Initial node registration error: %v (will retry)", err)
	}

	// 2. Launch Node Status / Capacity Sync Loop
	go c.runCapacitySyncLoop(ctx)

	// 3. Launch Lease Heartbeat Loop
	go c.runLeaseLoop(ctx)

	return nil
}

func (c *NodeController) Stop() {
	close(c.stopCh)
}

func (c *NodeController) parseQuantityBytes(str string) int64 {
	str = strings.TrimSpace(str)
	if str == "" {
		return 0
	}
	if q, err := resource.ParseQuantity(str); err == nil {
		return q.Value()
	}
	// Fallback custom suffix parser
	multiplier := int64(1)
	cleanStr := str
	if strings.HasSuffix(str, "Ki") {
		multiplier = 1024
		cleanStr = strings.TrimSuffix(str, "Ki")
	} else if strings.HasSuffix(str, "Mi") {
		multiplier = 1024 * 1024
		cleanStr = strings.TrimSuffix(str, "Mi")
	} else if strings.HasSuffix(str, "Gi") {
		multiplier = 1024 * 1024 * 1024
		cleanStr = strings.TrimSuffix(str, "Gi")
	}
	val, _ := strconv.ParseInt(cleanStr, 10, 64)
	return val * multiplier
}

func (c *NodeController) parseQuantityInt(str string) int64 {
	str = strings.TrimSpace(str)
	if str == "" {
		return 0
	}
	val, _ := strconv.ParseInt(str, 10, 64)
	return val
}

func (c *NodeController) AggregateCapacity(ctx context.Context) (corev1.ResourceList, corev1.ResourceList, bool, error) {
	nodes, err := c.k32Client.GetNodes(ctx)
	if err != nil {
		return nil, nil, false, err
	}

	c.mu.Lock()
	c.lastK32Success = time.Now()
	c.mu.Unlock()

	var totalCapMem, totalAllocMem int64
	var totalCapCPU, totalAllocCPU int64
	var totalCapGPIO, totalAllocGPIO int64
	var totalCapI2C, totalAllocI2C int64
	var totalCapADC, totalAllocADC int64
	anyNodeReady := false

	for _, n := range nodes {
		// Memory (Heap + PSRAM)
		heapCap := c.parseQuantityBytes(n.Status.Capacity["memory"])
		heapAlloc := c.parseQuantityBytes(n.Status.Allocatable["memory"])
		psramCap := c.parseQuantityBytes(n.Status.Capacity["esp32.io/psram"])
		if psramCap == 0 {
			psramCap = c.parseQuantityBytes(n.Status.Capacity["esp.io/psram"])
		}
		psramAlloc := c.parseQuantityBytes(n.Status.Allocatable["esp32.io/psram"])
		if psramAlloc == 0 {
			psramAlloc = c.parseQuantityBytes(n.Status.Allocatable["esp.io/psram"])
		}

		totalCapMem += (heapCap + psramCap)
		totalAllocMem += (heapAlloc + psramAlloc)

		// CPU
		cpuCap := c.parseQuantityInt(n.Status.Capacity["cpu"])
		if cpuCap == 0 {
			cpuCap = 2 // ESP32-S3 default 2 cores
		}
		cpuAlloc := c.parseQuantityInt(n.Status.Allocatable["cpu"])
		if cpuAlloc == 0 {
			cpuAlloc = cpuCap
		}
		totalCapCPU += cpuCap
		totalAllocCPU += cpuAlloc

		// Extended Peripherals
		gpioCap := c.parseQuantityInt(n.Status.Capacity["esp32.io/gpio"])
		if gpioCap == 0 {
			gpioCap = c.parseQuantityInt(n.Status.Capacity["esp.io/gpio"])
		}
		gpioAlloc := c.parseQuantityInt(n.Status.Allocatable["esp32.io/gpio"])
		if gpioAlloc == 0 {
			gpioAlloc = c.parseQuantityInt(n.Status.Allocatable["esp.io/gpio"])
		}
		totalCapGPIO += gpioCap
		totalAllocGPIO += gpioAlloc

		i2cCap := c.parseQuantityInt(n.Status.Capacity["esp32.io/i2c"])
		if i2cCap == 0 {
			i2cCap = c.parseQuantityInt(n.Status.Capacity["esp.io/i2c"])
		}
		i2cAlloc := c.parseQuantityInt(n.Status.Allocatable["esp32.io/i2c"])
		if i2cAlloc == 0 {
			i2cAlloc = c.parseQuantityInt(n.Status.Allocatable["esp.io/i2c"])
		}
		totalCapI2C += i2cCap
		totalAllocI2C += i2cAlloc

		adcCap := c.parseQuantityInt(n.Status.Capacity["esp32.io/adc"])
		if adcCap == 0 {
			adcCap = c.parseQuantityInt(n.Status.Capacity["esp.io/adc"])
		}
		adcAlloc := c.parseQuantityInt(n.Status.Allocatable["esp32.io/adc"])
		if adcAlloc == 0 {
			adcAlloc = c.parseQuantityInt(n.Status.Allocatable["esp.io/adc"])
		}
		totalCapADC += adcCap
		totalAllocADC += adcAlloc

		// Ready Condition
		for _, cond := range n.Status.Conditions {
			if cond.Type == "Ready" && cond.Status == "True" {
				anyNodeReady = true
				break
			}
		}
	}

	capList := corev1.ResourceList{
		corev1.ResourceCPU:              *resource.NewQuantity(totalCapCPU, resource.DecimalSI),
		corev1.ResourceMemory:           *resource.NewQuantity(totalCapMem, resource.BinarySI),
		corev1.ResourcePods:             *resource.NewQuantity(50, resource.DecimalSI),
		corev1.ResourceName("esp32.io/gpio"): *resource.NewQuantity(totalCapGPIO, resource.DecimalSI),
		corev1.ResourceName("esp.io/gpio"):   *resource.NewQuantity(totalCapGPIO, resource.DecimalSI),
		corev1.ResourceName("esp32.io/i2c"):  *resource.NewQuantity(totalCapI2C, resource.DecimalSI),
		corev1.ResourceName("esp32.io/adc"):  *resource.NewQuantity(totalCapADC, resource.DecimalSI),
	}

	allocList := corev1.ResourceList{
		corev1.ResourceCPU:              *resource.NewQuantity(totalAllocCPU, resource.DecimalSI),
		corev1.ResourceMemory:           *resource.NewQuantity(totalAllocMem, resource.BinarySI),
		corev1.ResourcePods:             *resource.NewQuantity(50, resource.DecimalSI),
		corev1.ResourceName("esp32.io/gpio"): *resource.NewQuantity(totalAllocGPIO, resource.DecimalSI),
		corev1.ResourceName("esp.io/gpio"):   *resource.NewQuantity(totalAllocGPIO, resource.DecimalSI),
		corev1.ResourceName("esp32.io/i2c"):  *resource.NewQuantity(totalAllocI2C, resource.DecimalSI),
		corev1.ResourceName("esp32.io/adc"):  *resource.NewQuantity(totalAllocADC, resource.DecimalSI),
	}

	return capList, allocList, anyNodeReady, nil
}

func (c *NodeController) reconcileNode(ctx context.Context) error {
	capList, allocList, anyReady, err := c.AggregateCapacity(ctx)
	isDisconnected := false
	if err != nil {
		c.mu.RLock()
		disconnectedDuration := time.Since(c.lastK32Success)
		c.mu.RUnlock()
		if disconnectedDuration > c.cfg.DisconnectTimeout {
			isDisconnected = true
		}
	}

	readyCondition := corev1.NodeCondition{
		Type:               corev1.NodeReady,
		Status:             corev1.ConditionTrue,
		LastHeartbeatTime:  metav1.Now(),
		LastTransitionTime: metav1.Now(),
		Reason:             "KubeletReady",
		Message:            "kube32 virtual kubelet edge gateway is operational",
	}

	if isDisconnected || (!anyReady && err == nil) {
		readyCondition.Status = corev1.ConditionFalse
		readyCondition.Reason = "Kube32ClusterUnreachable"
		readyCondition.Message = "downstream Kube32 mesh is unreachable or has no ready nodes"
	}

	existingNode, err := c.kubeClient.CoreV1().Nodes().Get(ctx, c.cfg.NodeName, metav1.GetOptions{})
	if apierrors.IsNotFound(err) {
		// Create new Node
		newNode := &corev1.Node{
			ObjectMeta: metav1.ObjectMeta{
				Name: c.cfg.NodeName,
				Labels: map[string]string{
					"kubernetes.io/hostname":         c.cfg.NodeName,
					"kubernetes.io/role":             "agent",
					"node-role.kubernetes.io/edge":   "",
					"node-role.kubernetes.io/worker": "",
					"type":                           "virtual-kubelet",
					"kubernetes.io/os":               "linux",
					"kubernetes.io/arch":             "xtensa",
					"kube32.io/federation":           "active",
				},
				Annotations: map[string]string{
					"node.alpha.kubernetes.io/ttl": "0",
					"kube32.io/federation-version": "v1.0.0",
				},
			},
			Spec: corev1.NodeSpec{
				Taints: []corev1.Taint{
					{
						Key:    "virtual-kubelet.io/provider",
						Value:  "kube32",
						Effect: corev1.TaintEffectNoSchedule,
					},
				},
			},
			Status: corev1.NodeStatus{
				NodeInfo: corev1.NodeSystemInfo{
					MachineID:               "kube32-cluster-edge-01",
					SystemUUID:              "00000000-0000-4000-8000-000000000002",
					BootID:                  "00000000-0000-0000-0000-000000000001",
					KernelVersion:           "FreeRTOS-v10.4",
					OSImage:                 "ESP-IDF v5.2 / Kube32",
					ContainerRuntimeVersion: "kube32-runtime://0.1.0",
					KubeletVersion:          "v1.31.0-kube32",
					OperatingSystem:         "linux",
					Architecture:            "xtensa",
				},
				Capacity:    capList,
				Allocatable: allocList,
				Conditions:  []corev1.NodeCondition{readyCondition},
			},
		}
		_, createErr := c.kubeClient.CoreV1().Nodes().Create(ctx, newNode, metav1.CreateOptions{})
		if createErr != nil {
			return fmt.Errorf("create node error: %w", createErr)
		}
		log.Printf("[NODE-CTRL] Registered upstream virtual node %s", c.cfg.NodeName)
		return nil
	} else if err != nil {
		return fmt.Errorf("get node error: %w", err)
	}

	// Update existing node status
	existingNode.Status.Capacity = capList
	existingNode.Status.Allocatable = allocList

	// Update ready condition
	updatedConditions := make([]corev1.NodeCondition, 0, len(existingNode.Status.Conditions)+1)
	foundReady := false
	for _, cond := range existingNode.Status.Conditions {
		if cond.Type == corev1.NodeReady {
			cond.Status = readyCondition.Status
			cond.Reason = readyCondition.Reason
			cond.Message = readyCondition.Message
			cond.LastHeartbeatTime = metav1.Now()
			foundReady = true
		}
		updatedConditions = append(updatedConditions, cond)
	}
	if !foundReady {
		updatedConditions = append(updatedConditions, readyCondition)
	}
	existingNode.Status.Conditions = updatedConditions

	_, updateErr := c.kubeClient.CoreV1().Nodes().UpdateStatus(ctx, existingNode, metav1.UpdateOptions{})
	if updateErr != nil {
		return fmt.Errorf("update node status error: %w", updateErr)
	}
	return nil
}

func (c *NodeController) runCapacitySyncLoop(ctx context.Context) {
	ticker := time.NewTicker(c.cfg.SyncInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-c.stopCh:
			return
		case <-ticker.C:
			if err := c.reconcileNode(ctx); err != nil {
				log.Printf("[NODE-CTRL] Capacity sync warning: %v", err)
			}
		}
	}
}

func (c *NodeController) runLeaseLoop(ctx context.Context) {
	ticker := time.NewTicker(c.cfg.LeaseInterval)
	defer ticker.Stop()

	// Initial immediate renewal
	_ = c.renewLease(ctx)

	for {
		select {
		case <-ctx.Done():
			return
		case <-c.stopCh:
			return
		case <-ticker.C:
			if err := c.renewLease(ctx); err != nil {
				log.Printf("[NODE-CTRL] Lease renewal warning: %v", err)
			}
		}
	}
}

func (c *NodeController) renewLease(ctx context.Context) error {
	c.mu.RLock()
	disconnected := time.Since(c.lastK32Success) > c.cfg.DisconnectTimeout
	c.mu.RUnlock()

	if disconnected {
		log.Printf("[NODE-CTRL] Kube32 disconnected > %v; skipping lease renewal", c.cfg.DisconnectTimeout)
		return nil
	}

	leaseClient := c.kubeClient.CoordinationV1().Leases("kube-node-lease")
	now := metav1.NewMicroTime(time.Now())
	durationSeconds := int32(40)

	existingLease, err := leaseClient.Get(ctx, c.cfg.NodeName, metav1.GetOptions{})
	if apierrors.IsNotFound(err) {
		newLease := &coordinationv1.Lease{
			ObjectMeta: metav1.ObjectMeta{
				Name:      c.cfg.NodeName,
				Namespace: "kube-node-lease",
			},
			Spec: coordinationv1.LeaseSpec{
				HolderIdentity:       &c.cfg.NodeName,
				LeaseDurationSeconds: &durationSeconds,
				RenewTime:            &now,
			},
		}
		_, createErr := leaseClient.Create(ctx, newLease, metav1.CreateOptions{})
		if createErr != nil {
			return fmt.Errorf("create lease failed: %w", createErr)
		}
		log.Printf("[NODE-CTRL] Created upstream lease for %s", c.cfg.NodeName)
		return nil
	} else if err != nil {
		return fmt.Errorf("get lease failed: %w", err)
	}

	existingLease.Spec.RenewTime = &now
	existingLease.Spec.HolderIdentity = &c.cfg.NodeName
	existingLease.Spec.LeaseDurationSeconds = &durationSeconds

	_, updateErr := leaseClient.Update(ctx, existingLease, metav1.UpdateOptions{})
	if updateErr != nil {
		if apierrors.IsConflict(updateErr) {
			// Optimistic concurrency retry
			latest, getErr := leaseClient.Get(ctx, c.cfg.NodeName, metav1.GetOptions{})
			if getErr == nil {
				latest.Spec.RenewTime = &now
				_, retryErr := leaseClient.Update(ctx, latest, metav1.UpdateOptions{})
				return retryErr
			}
		}
		return fmt.Errorf("update lease failed: %w", updateErr)
	}

	return nil
}
