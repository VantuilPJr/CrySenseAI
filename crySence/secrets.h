// =============================================================================
// CrySense AI v2.0 — secrets.h
// ATENÇÃO: Não versionar este arquivo no GitHub!
// Adicione "secrets.h" ao seu .gitignore
// =============================================================================
#pragma once

// --- WiFi ---
#define WIFI_SSID     "VIVOFIBRA-9C62"
#define WIFI_PASS     "0Z37012649"

// --- Firebase RTDB ---
// URL: encontrado em Firebase Console > Realtime Database > Data (sem / final)
#define FIREBASE_URL  "https://crysense-ai-default-rtdb.firebaseio.com"

// --- Firebase Auth ---
// Use a "Database Secret" (Console > Config > Contas de serviço > Secrets legados)
// OU uma API Key do Web App
#define FIREBASE_AUTH "AcZSfRPbAScn5F9cVc7yDRetJ3HNdD1qlEdhKYsq"

// --- Áudio via URL (opcional) ---
// URL do arquivo WAV de chuva/ruído branco (HTTP puro, sem HTTPS)
// Deixe vazio "" para usar apenas SPIFFS/sintético
#define AUDIO_CLOUD_URL "http://exemplo.com/chuva.wav"
