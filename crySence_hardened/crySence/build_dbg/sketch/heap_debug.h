#line 1 "C:\\Arduino\\crySense_ai\\crySence_hardened\\crySence\\heap_debug.h"
// =============================================================================
// CrySense AI v2.0 — heap_debug.h
// Detecção de corrupção de heap com isolamento de tasks
// Uso: #define DEBUG_HEAP_CORRUPTION 1 para ativar detecção ativa
// =============================================================================
#pragma once

// =============================================================================
// CONTROLE DE DEBUG: Desabilitar tasks para isolar corruption
// =============================================================================
// Mude para 1 para DESABILITAR cada task durante debug
#define DISABLE_TASKIA    0
#define DISABLE_TASKAUDIO 0
#define DISABLE_TASKIOT   0
#define DISABLE_TASKHMI   0
#define DISABLE_TASKDEC   0

// Ativar detecção ativa de corrupção (rodar a cada ~5s)
// DISABLED: consume demais stack com 101KB DRAM crítico
#define DEBUG_HEAP_CORRUPTION 0

// Nível de log de heap:
//   0 = apenas erros críticos
//   1 = avisos + estatísticas básicas
//   2 = trace detalhado (PESADO em CPU)
#define HEAP_DEBUG_LEVEL 1

// =============================================================================
// HELPER — Captura snapshot de heap
// =============================================================================
struct HeapSnapshot {
  uint32_t free_psram;
  uint32_t free_dram;
  uint32_t total_alloc;
};

inline HeapSnapshot heap_snapshot() {
  HeapSnapshot snap = {};
  snap.free_psram = ESP.getFreePsram();
  snap.free_dram = ESP.getFreeHeap();
  snap.total_alloc = 0; // Não temos forma simples de medir alocação total
  return snap;
}

inline void heap_print_snapshot(const char *label, const HeapSnapshot &snap) {
  Serial.printf("[HEAP-SNAP] %s: PSRAM_free=%u KB | DRAM_free=%u KB\n",
                label,
                snap.free_psram / 1024,
                snap.free_dram / 1024);
}

// =============================================================================
// CHECKPOINT — Detectar queda suspeita de heap entre dois snapshots
// =============================================================================
inline bool heap_check_corruption(const HeapSnapshot &before, const HeapSnapshot &after) {
  // Heurística: Se heap caiu mais de 50% de repente, pode indicar corrupção
  uint32_t dram_drop_pct = ((before.free_dram - after.free_dram) * 100) / (before.free_dram + 1);
  uint32_t psram_drop_pct = ((before.free_psram - after.free_psram) * 100) / (before.free_psram + 1);
  
  bool dram_suspicious = (dram_drop_pct > 50) && (after.free_dram < 4096);
  bool psram_suspicious = (psram_drop_pct > 50) && (after.free_psram < 8192);
  
  if (dram_suspicious || psram_suspicious) {
    Serial.printf("[HEAP-WARN] Queda suspeita! PSRAM: %u -> %u KB (-%u%%), DRAM: %u -> %u KB (-%u%%)\n",
                  before.free_psram / 1024,
                  after.free_psram / 1024,
                  psram_drop_pct,
                  before.free_dram / 1024,
                  after.free_dram / 1024,
                  dram_drop_pct);
    return true;
  }
  
  return false;
}

// =============================================================================
// CHECK INTEGRIDADE — Snapshot + heurísticas para detectar corrupção
// Versão simplificada que não depende de funções complexas do SDK
// =============================================================================
inline bool heap_integrity_check(const char *label) {
#if DEBUG_HEAP_CORRUPTION
  // Usar heurísticas simples: se heap < 2KB, situação crítica
  HeapSnapshot snap = heap_snapshot();
  
  bool dram_ok = (snap.free_dram >= 2048);
  bool psram_ok = (snap.free_psram >= 4096);
  
  if (!dram_ok) {
    Serial.printf("[HEAP-ERROR] %s: DRAM CRÍTICO! free=%u bytes\n", label, snap.free_dram);
  }
  
  if (!psram_ok) {
    Serial.printf("[HEAP-ERROR] %s: PSRAM CRÍTICO! free=%u bytes\n", label, snap.free_psram);
  }
  
  if ((dram_ok && psram_ok) && HEAP_DEBUG_LEVEL >= 2) {
    Serial.printf("[HEAP-OK] %s: Memória OK (%u/%u KB)\n", label, snap.free_dram/1024, snap.free_psram/1024);
  }
  
  return (dram_ok && psram_ok);
#else
  return true;
#endif
}

// =============================================================================
// MACRO — Wrapper para logging detalhado antes/depois de operações críticas
// =============================================================================
#define HEAP_DEBUG_START(label) \
  HeapSnapshot _heap_snap_start = heap_snapshot(); \
  if (HEAP_DEBUG_LEVEL >= 1) { \
    Serial.printf("[HEAP-BEGIN] %s\n", label); \
  }

#define HEAP_DEBUG_END(label) \
  HeapSnapshot _heap_snap_end = heap_snapshot(); \
  if (HEAP_DEBUG_LEVEL >= 1) { \
    heap_print_snapshot(label, _heap_snap_end); \
    heap_check_corruption(_heap_snap_start, _heap_snap_end); \
  }

// =============================================================================
// TASK CONTROLE — Wrapper para desabilitar tasks via define
// Uso: if (SHOULD_RUN_TASKIA) { /* task code */ }
// =============================================================================
#define SHOULD_RUN_TASKIA    (!DISABLE_TASKIA)
#define SHOULD_RUN_TASKAUDIO (!DISABLE_TASKAUDIO)
#define SHOULD_RUN_TASKIOT   (!DISABLE_TASKIOT)
#define SHOULD_RUN_TASKHMI   (!DISABLE_TASKHMI)
#define SHOULD_RUN_TASKDEC   (!DISABLE_TASKDEC)

// =============================================================================
// HANDLER PERIÓDICO — Executar em TaskHMI a cada 5s para monitorar heap
// =============================================================================
inline void heap_monitor_cycle() {
#if DEBUG_HEAP_CORRUPTION
  static uint32_t last_check_ms = 0;
  uint32_t now_ms = millis();
  
  // Verificar a cada 5 segundos
  if ((now_ms - last_check_ms) >= 5000) {
    last_check_ms = now_ms;
    
    Serial.println("\n=== HEAP MONITOR CICLO ===");
    HeapSnapshot snap = heap_snapshot();
    heap_print_snapshot("PERIODIC_CHECK", snap);
    
    // Rodar full integrity check (PESADO!)
    if (HEAP_DEBUG_LEVEL >= 1) {
      heap_integrity_check("PERIODIC_INTEGRITY");
    }
    
    Serial.println("=== FIM HEAP MONITOR ===\n");
  }
#endif
}
