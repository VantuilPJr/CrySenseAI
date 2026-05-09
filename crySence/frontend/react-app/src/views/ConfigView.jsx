import { useState, useEffect } from 'react';
import { Save, RotateCcw, UploadCloud } from 'lucide-react';
import toast from 'react-hot-toast';

export default function ConfigView({ config, onRefresh }) {
    const [formData, setFormData] = useState({
        wifi_ssid: '',
        wifi_pass: '',
        firebase_url: '',
        firebase_auth: '',
        audio_url: '',
        classifier_url: '',
        classifier_timeout_ms: 12000,
        confianca: 60,
        volume: 50,
        temp_max: 28,
        temp_min: 18,
        sensor_s: 5,
        sleep_min: 0,
        ota_password: ''
    });

    const [scannedNetworks, setScannedNetworks] = useState([]);
    const [isScanning, setIsScanning] = useState(false);
    const [scanDone, setScanDone] = useState(false);

    useEffect(() => {
        if (config) {
            setFormData(prev => ({
                ...prev,
                wifi_ssid: config.wifi_ssid || '',
                wifi_pass: '',
                firebase_url: config.firebase_url || '',
                firebase_auth: '',
                audio_url: config.audio_url || '',
                classifier_url: config.classifier_url || '',
                classifier_timeout_ms: config.classifier_timeout_ms ?? prev.classifier_timeout_ms,
                confianca: config.confianca_minima ? Math.round(config.confianca_minima * 100) : prev.confianca,
                volume: config.volume_audio ?? prev.volume,
                temp_max: config.temp_max_conforto ?? prev.temp_max,
                temp_min: config.temp_min_conforto ?? prev.temp_min,
                sensor_s: config.sensor_intervalo_ms ? Math.max(1, Math.round(config.sensor_intervalo_ms / 1000)) : prev.sensor_s,
                sleep_min: config.light_sleep_min ?? prev.sleep_min,
                ota_password: '' // Don't prefill with ****
            }));
        }
    }, [config]);

    const handleChange = (e) => {
        const { name, value } = e.target;
        setFormData(prev => ({ ...prev, [name]: value }));
    };

    const scanWifi = async () => {
        setIsScanning(true);
        const toastId = toast.loading('Espere: Buscando redes 2.4GHz...');
        try {
            let res = await fetch('/api/wifi/scan');
            if (res.status === 202) {
                let attempts = 0;
                while (attempts < 20) {
                    await new Promise(r => setTimeout(r, 1000));
                    res = await fetch('/api/wifi/scan/result');
                    if (res.status === 200) break;
                    if (res.status !== 202) throw new Error("Scan failed");
                    attempts++;
                }
            }
            if (res.ok && res.status === 200) {
                const data = await res.json();
                const clean = data.filter(n => n.ssid && n.ssid.trim() !== '').sort((a,b) => b.rssi - a.rssi);
                setScannedNetworks(clean);
                setScanDone(true);
                toast.success('Escaneamento concluído!', { id: toastId });
            } else {
                toast.error('Falha no escaneamento.', { id: toastId });
            }
        } catch (err) {
            console.error(err);
            toast.error('API indisponível. Simulador Local?', { id: toastId });
        } finally {
            setIsScanning(false);
        }
    };

    const salvarConfig = async (e) => {
        e.preventDefault();
        const toastId = toast.loading('Salvando no dispositivo...');
        try {
            const payload = {
                wifi_ssid: formData.wifi_ssid,
                wifi_pass: formData.wifi_pass ? formData.wifi_pass : '__UNCHANGED__',
                firebase_url: formData.firebase_url,
                firebase_auth: formData.firebase_auth ? formData.firebase_auth : '****',
                audio_url: formData.audio_url,
                classifier_url: formData.classifier_url,
                classifier_timeout_ms: Number(formData.classifier_timeout_ms),
                confianca_minima: Number(formData.confianca) / 100,
                volume_audio: Number(formData.volume),
                temp_max_conforto: Number(formData.temp_max),
                temp_min_conforto: Number(formData.temp_min),
                sensor_intervalo_ms: Number(formData.sensor_s) * 1000,
                light_sleep_min: Number(formData.sleep_min),
                ota_password: formData.ota_password ? formData.ota_password : '****',
            };

            const res = await fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });
            if (res.ok) {
                const out = await res.json();
                if (out.changed) {
                    toast.success('Configurações salvas! Reiniciando...', { id: toastId });
                } else {
                    toast.success('Nenhuma mudança detectada.', { id: toastId });
                }
                onRefresh?.();
            } else {
                toast.error('Falha ao salvar. Módulo online?', { id: toastId });
            }
        } catch (err) {
            console.error(err);
            toast.error('Network erro (Simulação local)', { id: toastId });
        }
    };

    const uploadWav = async () => {
        const file = document.getElementById('wavFile').files[0];
        if (!file) return;

        const toastId = toast.loading('Enviando arquivo via rede...');
        try {
            const buf = await file.arrayBuffer();
            const res = await fetch('/api/audio/upload', {
                method: 'POST',
                body: buf
            });
            if (res.ok) toast.success('Upload concluído com sucesso!', { id: toastId });
            else toast.error('Falha no upload (limite 1.5MB - SPIFFS)', { id: toastId });
        } catch (err) {
            console.error(err);
            toast.error('Erro de rede durante upload (Simulação).', { id: toastId });
        }
    };

    const resetConfig = () => {
        toast((t) => (
            <div>
                <p style={{ marginBottom: '8px' }}>Atenção: retornar config para os defaults do firmware?</p>
                <div style={{ display: 'flex', gap: '8px', justifyContent: 'flex-end' }}>
                    <button className="btn btn-danger" onClick={async () => {
                        toast.dismiss(t.id);
                        const toastId = toast.loading('Enviando reset...');
                        try {
                            const res = await fetch('/api/config/reset', { method: 'POST' });
                            if (res.ok) {
                                toast.success('Reset enviado. Reiniciando módulo...', { id: toastId });
                                onRefresh?.();
                            } else {
                                toast.error('Falha ao resetar configuração.', { id: toastId });
                            }
                        } catch (err) {
                            console.error(err);
                            toast.error('Erro de rede ao resetar.', { id: toastId });
                        }
                    }}>Resetar</button>
                    <button className="btn" onClick={() => toast.dismiss(t.id)}>Cancelar</button>
                </div>
            </div>
        ), { duration: Infinity });
    };

    return (
        <div className="tab-content fade-in" style={{ maxWidth: '800px' }}>
            <form onSubmit={salvarConfig} className="chart-card" style={{ padding: '32px' }}>

                <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '16px' }}>
                    <h3 style={{ color: 'var(--acc-blue-light)', margin: 0, display: 'flex', alignItems: 'center', gap: '8px' }}>
                        📶 Rede WiFi (sem reflash)
                    </h3>
                    <button type="button" className="btn btn-sm" onClick={scanWifi} disabled={isScanning} style={{ padding: '6px 12px', fontSize: '0.9rem' }}>
                       {isScanning ? '⏳ Buscando...' : '📡 Escanear Redes'}
                    </button>
                </div>
                <div className="form-group grid-cards" style={{ gridTemplateColumns: '1fr 1fr', marginBottom: '24px' }}>
                    <div>
                        <label>SSID (Nome da Rede)</label>
                        {scanDone && scannedNetworks.length > 0 ? (
                            <div style={{ display: 'flex', gap: '8px' }}>
                                <select 
                                    name="wifi_ssid" 
                                    value={formData.wifi_ssid} 
                                    onChange={handleChange} 
                                    style={{ flex: 1, padding: '10px', borderRadius: '8px', border: '1px solid var(--border-color)', background: 'var(--bg-card)', color: 'var(--text-main)', fontSize: '0.95rem' }}
                                >
                                    <option value="" disabled>Selecione a sua rede...</option>
                                    {scannedNetworks.map((n, i) => (
                                        <option key={i} value={n.ssid}>{n.ssid} (Sinal: {n.rssi} dBm)</option>
                                    ))}
                                </select>
                                <button type="button" className="btn" onClick={() => setScanDone(false)} title="Digitar Manualmente" style={{ padding: '0 12px' }}>✏️</button>
                            </div>
                        ) : (
                            <input type="text" name="wifi_ssid" value={formData.wifi_ssid} onChange={handleChange} maxLength={32} placeholder="Digite o SSID" />
                        )}
                    </div>
                    <div>
                        <label>Senha</label>
                        <input type="password" name="wifi_pass" value={formData.wifi_pass} onChange={handleChange} maxLength={64} placeholder="********" />
                    </div>
                </div>

                <h3 style={{ color: '#f59e0b', margin: '32px 0 16px', display: 'flex', alignItems: 'center', gap: '8px' }}>
                    🔥 Firebase RTDB
                </h3>
                <div className="form-group grid-cards" style={{ gridTemplateColumns: '1fr 1fr', marginBottom: '24px' }}>
                    <div>
                        <label>URL do RTDB</label>
                        <input type="url" name="firebase_url" value={formData.firebase_url} onChange={handleChange} placeholder="https://seu-projeto.firebaseio.com" />
                    </div>
                    <div>
                        <label>Auth / API Key</label>
                        <input type="text" name="firebase_auth" value={formData.firebase_auth} onChange={handleChange} placeholder="Chave secreta..." />
                    </div>
                </div>

                <h3 style={{ color: '#3b82f6', margin: '32px 0 16px' }}>🎵 Áudio para Cólica</h3>
                <div className="form-group">
                    <label>URL de áudio na nuvem (WAV HTTP mono 16kHz) — opcional</label>
                    <input type="url" name="audio_url" value={formData.audio_url} onChange={handleChange} placeholder="http://...chuva.wav" />
                </div>

                <h3 style={{ color: '#6366f1', margin: '32px 0 16px' }}>🧠 Classificação remota (Notebook)</h3>
                <div className="form-group grid-cards" style={{ gridTemplateColumns: '2fr 1fr', marginBottom: '24px' }}>
                    <div>
                        <label>URL do classificador remoto</label>
                        <input
                            type="url"
                            name="classifier_url"
                            value={formData.classifier_url}
                            onChange={handleChange}
                            placeholder="http://192.168.x.x:8000/classify"
                        />
                    </div>
                    <div>
                        <label>Timeout remoto (ms)</label>
                        <input
                            type="number"
                            name="classifier_timeout_ms"
                            value={formData.classifier_timeout_ms}
                            onChange={handleChange}
                            min="1000"
                            max="30000"
                            step="500"
                        />
                    </div>
                </div>

                <div className="form-group" style={{ background: 'var(--bg-base)', padding: '16px', borderRadius: '8px', border: '1px solid var(--border-color)', display: 'flex', gap: '16px', alignItems: 'flex-end', flexWrap: 'wrap' }}>
                    <div style={{ flex: 1, minWidth: '200px' }}>
                        <label>Upload WAV local (SPIFFS): máx. 1.5MB</label>
                        <input type="file" accept=".wav" id="wavFile" />
                    </div>
                    <button type="button" className="btn" onClick={uploadWav}>
                        <UploadCloud size={16} /> Enviar arquivo
                    </button>
                </div>
                
                <h3 style={{ color: '#ec4899', margin: '32px 0 16px' }}>🔒 Segurança (OTA)</h3>
                <div className="form-group grid-cards" style={{ gridTemplateColumns: '1fr', marginBottom: '24px' }}>
                    <div>
                        <label>Senha para Update Remoto (OTA)</label>
                        <input type="password" name="ota_password" value={formData.ota_password} onChange={handleChange} placeholder="********" />
                    </div>
                </div>

                <h3 style={{ color: '#a855f7', margin: '32px 0 16px' }}>⚙️ Parâmetros IA e Desempenho</h3>
                <div className="grid-cards" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(180px, 1fr))', marginBottom: '32px' }}>
                    <div>
                        <label>Confiança mínima (%)</label>
                        <input type="number" name="confianca" value={formData.confianca} onChange={handleChange} min="20" max="99" />
                    </div>
                    <div>
                        <label>Volume áudio (%)</label>
                        <input type="number" name="volume" value={formData.volume} onChange={handleChange} min="0" max="100" />
                    </div>
                    <div>
                        <label>Temp. máxima (°C)</label>
                        <input type="number" name="temp_max" value={formData.temp_max} onChange={handleChange} min="20" max="40" step="0.5" />
                    </div>
                    <div>
                        <label>Temp. mínima (°C)</label>
                        <input type="number" name="temp_min" value={formData.temp_min} onChange={handleChange} min="10" max="25" step="0.5" />
                    </div>
                    <div>
                        <label>Interv. sensores (s)</label>
                        <input type="number" name="sensor_s" value={formData.sensor_s} onChange={handleChange} min="1" max="60" />
                    </div>
                    <div>
                        <label>Light sleep (min, 0=off)</label>
                        <input type="number" name="sleep_min" value={formData.sleep_min} onChange={handleChange} min="0" max="60" />
                    </div>
                </div>

                <div style={{ display: 'flex', gap: '12px', paddingTop: '24px', borderTop: '1px solid var(--border-color)' }}>
                    <button type="submit" className="btn btn-primary" style={{ padding: '10px 24px' }}>
                        <Save size={18} /> Salvar e Reiniciar
                    </button>
                    <button type="button" className="btn btn-danger" onClick={resetConfig}>
                        <RotateCcw size={18} /> Reset Defaults
                    </button>
                </div>

            </form>
        </div>
    );
}
