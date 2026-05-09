#pragma once

// --- WiFi ---
#define WIFI_SSID "ESPJunior"
#define WIFI_PASS "12345678"
// --- Firebase RTDB ---
// URL: encontrado em Firebase Console > Realtime Database > Data (sem / final)
#define FIREBASE_URL "https://crysense-ai-default-rtdb.firebaseio.com"

// --- Firebase Auth ---
// Use a "Database Secret" (Console > Config > Contas de serviço > Secrets
// legados) OU uma API Key do Web App
#define FIREBASE_AUTH "AcZSfRPbAScn5F9cVc7yDRetJ3HNdD1qlEdhKYsq"

// --- Áudio via URL (opcional) ---
// URL do arquivo WAV de chuva/ruído branco (HTTP puro, sem HTTPS)
// Deixe vazio "" para usar apenas SPIFFS/sintético
#define AUDIO_CLOUD_URL "http://exemplo.com/chuva.wav"


// --- Servidor de Classificação Remota ---
// IP do hotspot do celular (rede atual verificada): 10.108.70.59
// Endpoint: http://10.108.70.59:8000/classify
//
// Para iniciar o servidor (notebook conectado ao mesmo hotspot):
//   Set-Location C:\Arduino\crySense_ai\crySence\notebook_classifier
//   python -m uvicorn server:app --host 0.0.0.0 --port 8000
//
// ATENÇÃO: O ESP32 e o notebook devem estar na MESMA rede (mesmo hotspot).
