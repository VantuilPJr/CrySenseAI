import { useState, useEffect, useCallback } from 'react';

const MAX_HISTORY = 60;

export function useDashboardData() {
    const [data, setData] = useState({});
    const [history, setHistory] = useState({
        heap: [],
        inf: [],
        temp: [],
        umid: [],
        pres: [],   // pressão atmosférica
    });
    const [analytics, setAnalytics] = useState({ eventos: [], resumo: {} });
    const [logs, setLogs] = useState([]);
    const [config, setConfig] = useState({});

    const isPageVisible = () => typeof document === 'undefined' || document.visibilityState === 'visible';

    const pushHistory = (arr, val) => {
        const newArr = [...arr, val];
        if (newArr.length > MAX_HISTORY) newArr.shift();
        return newArr;
    };

    // Polling de status + sensores a cada 1s
    const fetchPerf = useCallback(async () => {
        if (!isPageVisible()) return;
        try {
            const res = await fetch('/api/status');
            if (res.ok) {
                const d = await res.json();
                setData(d);
                setHistory(prev => {
                    const h = { ...prev };
                    if (d.heap  !== undefined) h.heap = pushHistory(prev.heap, d.heap / 1024);
                    if (d.inf_ms !== undefined) h.inf  = pushHistory(prev.inf, d.inf_ms);
                    if (d.temp  !== undefined) {
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

    // Analytics — carregado sob demanda (tab Análises) e a cada 30s
    const fetchAnalytics = useCallback(async () => {
        if (!isPageVisible()) return;
        try {
            const res = await fetch('/api/analytics');
            if (res.ok) setAnalytics(await res.json());
        } catch (err) {
            console.error('Falha ao buscar analytics', err);
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

    useEffect(() => {
        fetchPerf();
        const interval = setInterval(fetchPerf, 1500);
        return () => clearInterval(interval);
    }, [fetchPerf]);

    // Atualiza analytics a cada 30s em background
    useEffect(() => {
        fetchAnalytics();
        const interval = setInterval(fetchAnalytics, 30000);
        return () => clearInterval(interval);
    }, [fetchAnalytics]);

    return { data, history, analytics, logs, config, fetchLogs, fetchConfig, fetchAnalytics };
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
