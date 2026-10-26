package syncer

import (
	"context"
	"fmt"
	"log"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"

	"github.com/arterialist/kube32/federation/pkg/client"
)

type DeletionSyncer struct {
	cfg        *SyncerConfig
	kubeClient kubernetes.Interface
	k32Client  *client.Kube32Client
	stopCh     chan struct{}
}

func NewDeletionSyncer(cfg *SyncerConfig, kubeClient kubernetes.Interface, k32Client *client.Kube32Client) *DeletionSyncer {
	if cfg.SyncInterval <= 0 {
		cfg.SyncInterval = 2 * time.Second
	}
	return &DeletionSyncer{
		cfg:        cfg,
		kubeClient: kubeClient,
		k32Client:  k32Client,
		stopCh:     make(chan struct{}),
	}
}

func (s *DeletionSyncer) Start(ctx context.Context) {
	go s.runDeletionLoop(ctx)
}

func (s *DeletionSyncer) Stop() {
	close(s.stopCh)
}

func (s *DeletionSyncer) ReconcileDeletion(ctx context.Context, pod *corev1.Pod) error {
	if pod.Spec.NodeName != s.cfg.VirtualNodeName {
		return nil
	}

	// Only process pods that have DeletionTimestamp set
	if pod.DeletionTimestamp == nil {
		return nil
	}

	ns := pod.Namespace
	name := pod.Name
	key := fmt.Sprintf("%s/%s", ns, name)

	hasFinalizer := false
	for _, f := range pod.Finalizers {
		if f == FederationFinalizer {
			hasFinalizer = true
			break
		}
	}

	if !hasFinalizer {
		return nil // Finalizer already stripped or never added
	}

	log.Printf("[DELETION-SYNC] Detected deletion for %s (gracePeriod=%v)", key, pod.DeletionGracePeriodSeconds)

	// 1. Issue DELETE to downstream Kube32
	err := s.k32Client.DeletePod(ctx, ns, name)
	if err != nil {
		return fmt.Errorf("failed to delete downstream pod %s: %w", key, err)
	}
	log.Printf("[DELETION-SYNC] Successfully deleted downstream pod %s from Kube32", key)

	// 2. Remove finalizer from upstream pod
	newFinalizers := make([]string, 0, len(pod.Finalizers))
	for _, f := range pod.Finalizers {
		if f != FederationFinalizer {
			newFinalizers = append(newFinalizers, f)
		}
	}

	podCopy := pod.DeepCopy()
	podCopy.Finalizers = newFinalizers

	_, err = s.kubeClient.CoreV1().Pods(ns).Update(ctx, podCopy, metav1.UpdateOptions{})
	if err != nil {
		return fmt.Errorf("failed to remove finalizer from upstream pod %s: %w", key, err)
	}

	log.Printf("[DELETION-SYNC] Removed finalizer from upstream pod %s; purge finalized", key)
	return nil
}

func (s *DeletionSyncer) runDeletionLoop(ctx context.Context) {
	ticker := time.NewTicker(s.cfg.SyncInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-s.stopCh:
			return
		case <-ticker.C:
			pods, err := s.kubeClient.CoreV1().Pods("").List(ctx, metav1.ListOptions{})
			if err != nil {
				continue
			}

			for i := range pods.Items {
				pod := &pods.Items[i]
				if pod.DeletionTimestamp != nil {
					if err := s.ReconcileDeletion(ctx, pod); err != nil {
						log.Printf("[DELETION-SYNC] Error reconciling deletion for %s/%s: %v", pod.Namespace, pod.Name, err)
					}
				}
			}
		}
	}
}
