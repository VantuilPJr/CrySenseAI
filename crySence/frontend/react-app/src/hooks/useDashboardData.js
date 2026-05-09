import { useState, useEffect, useCallback } from 'react';

const MAX_HISTORY = 60; // histórico em-memória do browser (últimos 60 ciclos de polling)

export function useDashboardData() {
    const [data, setData] = useState({});
    const [history, setHistory] = useState({
        heap: [],
        inf: [],
        temp: [],
        umid: [],
        pres: [],
    });
    const [analytics, setAnalytics] = useState({ eventos: [], resumo: {} });
    const [sensorHistory, setSensorHistory] = useState([]); // Dados históricos 24h do ESP32
    const [logs, setLogs] = useState([]);
    const [config, setConfig] = useState({});

    const isPageVisible = () => typeof document === 'undefined' || document.visibilityState === 'visible';

    const pushHistory = (arr, val) => {
        const newArr = [...arr, val];
        if (newArr.length > MAX_HISTORY) newArr.shift();
        return newArr;
    };

    // Polling de status + sensores a cada 1.5s
    const fetchPerf = useCallback(async () => {
        if (!isPageVisible()) return;
        try {
            const res = await fetch('/api/status');
            if (res.ok) {
                const d = await res.json();
                setData(d);
                setHistory(prev => {
                    const h = { ...prev };
                    if (d.heap   !== undefined) h.heap = pushHistory(prev.heap, d.heap / 1024);
                    if (d.inf_ms !== undefined) h.inf  = pushHistory(prev.inf, d.inf_ms);
                    if (d.temp   !== undefined) {
                        h.temp = pushHistory(prev.temp, parseFloat(d.temp));
                        h.umid = pushHistory(prev.umid, parseFloat(d.umid || 0));
                        h.pres = pushHistory(prev.pres, parseFloat(d.pres || 0));
                    }
                    return h;
                });
            }
        } catch (err) {
            console.error('Falha ao buscar status', err);
        }
    }, []);

    // Analytics (episódios de choro) — sob demanda e a cada 30s
    const fetchAnalytics = useCallback(async () => {
        if (!isPageVisible()) return;
        try {
            const res = await fetch('/api/analytics');
            if (res.ok) setAnalytics(await res.json());
        } catch (err) {
            console.error('Falha ao buscar analytics', err);
        }
    }, []);

    // Histórico 24h de sensores + IA — lido da PSRAM do ESP32 (zero custo para Flash)
    const fetchSensorHistory = useCallback(async () => {
        if (!isPageVisible()) return;
        try {
            const res = await fetch('/api/history?limit=2880');
            if (res.ok) setSensorHistory(await res.json());
        } catch (err) {
            console.error('Falha ao buscar histórico de sensores', err);
        }
    }, []);

    const fetchLogs = useCallback(async () => {
        try {
            const res = await fetch('/api/logs');
            if (res.ok) setLogs(await res.json());
        } catch (err) {
            console.error('Falha ao buscar logs', err);
        }
    }, []);

    const fetchConfig = useCallback(async () => {
        try {
            const res = await fetch('/api/config');
            if (res.ok) setConfig(await res.json());
        } catch (err) {
            console.error('Falha ao buscar config', err);
        }
    }, []);

    // Status a cada 1.5s
    useEffect(() => {
        fetchPerf();
        const interval = setInterval(fetchPerf, 1500);
        return () => clearInterval(interval);
    }, [fetchPerf]);

    // Analytics a cada 30s em background
    useEffect(() => {
        fetchAnalytics();
        const interval = setInterval(fetchAnalytics, 30000);
        return () => clearInterval(interval);
    }, [fetchAnalytics]);

    // Histórico de sensores a cada 60s (o ESP32 grava a cada 30s)
    useEffect(() => {
        fetchSensorHistory();
        const interval = setInterval(fetchSensorHistory, 60000);
        return () => clearInterval(interval);
    }, [fetchSensorHistory]);

    return { data, history, sensorHistory, analytics, logs, config,
             fetchLogs, fetchConfig, fetchAnalytics, fetchSensorHistory };
}

export function formatUptime(s) {
    s = s || 0;
    const h   = String(Math.floor(s / 3600)).padStart(2, '0');
    const m   = String(Math.floor((s % 3600) / 60)).padStart(2, '0');
    const sec = String(s % 60).padStart(2, '0');
    return `${h}:${m}:${sec}`;
}

// Formata segundos de uptime em "Xmin atrás" relativo
export function formatRelTime(ts_s, uptime_s) {
    const diffMin = Math.round((uptime_s - ts_s) / 60);
    if (diffMin < 1)  return 'agora';
    if (diffMin < 60) return `${diffMin}min atrás`;
    const h = Math.floor(diffMin / 60);
    const m = diffMin % 60;
    return `${h}h${m > 0 ? m + 'min' : ''} atrás`;
}
