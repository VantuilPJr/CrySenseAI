import { useRef, useEffect } from 'react';
import LineChartCard from '../components/LineChartCard';
import Skeleton from '../components/Skeleton';
import { formatRelTime } from '../hooks/useDashboardData';

// Gráfico de linha histórico nativo via Canvas — sem dependências externas
// Suporta múltiplas séries e eixo X com labels de tempo relativo
function HistoryLineChart({ samples, series, title, uptime_s }) {
    const ref = useRef(null);

    useEffect(() => {
        const canvas = ref.current;
        if (!canvas || !samples || samples.length < 2) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.offsetWidth || 600;
        const H = canvas.height;
        canvas.width = W;
        const PAD = { l: 48, r: 16, t: 20, b: 32 };
        const IW = W - PAD.l - PAD.r;
        const IH = H - PAD.t - PAD.b;

        const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
        const gridColor  = isDark ? '#30363d' : '#e2e8f0';
        const textColor  = isDark ? '#8b949e' : '#64748b';
        const bgColor    = isDark ? '#161b22' : '#ffffff';

        ctx.fillStyle = bgColor;
        ctx.fillRect(0, 0, W, H);

        // Calcula min/max de todos os valores das séries
        let allVals = [];
        series.forEach(s => { allVals.push(...samples.map(p => p[s.key])); });
        allVals = allVals.filter(v => v !== undefined && !isNaN(v));
        if (allVals.length === 0) return;
        let minVal = Math.min(...allVals);
        let maxVal = Math.max(...allVals);
        if (maxVal === minVal) { minVal -= 1; maxVal += 1; }
        const valRange = maxVal - minVal;

        // Eixo X por timestamp
        const minTs = samples[0].ts;
        const maxTs = samples[samples.length - 1].ts;
        const tsRange = maxTs - minTs || 1;

        // Grid horizontal (5 linhas)
        for (let i = 0; i <= 4; i++) {
            const v = minVal + (i / 4) * valRange;
            const y = PAD.t + IH - ((v - minVal) / valRange) * IH;
            ctx.strokeStyle = gridColor;
            ctx.lineWidth = 1;
            ctx.setLineDash([3, 3]);
            ctx.beginPath(); ctx.moveTo(PAD.l, y); ctx.lineTo(PAD.l + IW, y); ctx.stroke();
            ctx.fillStyle = textColor;
            ctx.font = '10px sans-serif';
            ctx.fillText(v.toFixed(1), 2, y + 4);
        }
        ctx.setLineDash([]);

        // Desenha cada série como linha
        series.forEach(s => {
            ctx.beginPath();
            ctx.strokeStyle = s.color;
            ctx.lineWidth = 1.8;
            let first = true;
            samples.forEach(p => {
                const val = p[s.key];
                if (val === undefined || isNaN(val)) return;
                const x = PAD.l + ((p.ts - minTs) / tsRange) * IW;
                const y = PAD.t + IH - ((val - minVal) / valRange) * IH;
                if (first) { ctx.moveTo(x, y); first = false; }
                else ctx.lineTo(x, y);
            });
            ctx.stroke();
        });

        // Labels eixo X (5 pontos de tempo)
        ctx.fillStyle = textColor;
        ctx.font = '9px sans-serif';
        for (let i = 0; i <= 4; i++) {
            const ts = minTs + (i / 4) * tsRange;
            const x  = PAD.l + (i / 4) * IW;
            const label = formatRelTime(ts, uptime_s);
            ctx.fillText(label, x - 14, H - 6);
        }
    }, [samples, series, uptime_s]);

    return (
        <div style={{ position: 'relative' }}>
            <canvas
                ref={ref}
                height={180}
                style={{ width: '100%', height: 180, borderRadius: 8, display: 'block' }}
            />
            {samples && samples.length < 2 && (
                <div style={{
                    position: 'absolute', inset: 0, display: 'flex', alignItems: 'center',
                    justifyContent: 'center', flexDirection: 'column', gap: 8,
                    color: 'var(--text-muted)', fontSize: '0.85rem'
                }}>
                    <span style={{ fontSize: '1.5rem' }}>⏳</span>
                    Aguardando dados... (amostra a cada 30s)
                </div>
            )}
        </div>
    );
}

export default function SensorsView({ data, history, sensorHistory = [] }) {
    const uptime_s = data.uptime_s || 0;
    const conforto = data.conforto_ok;
    const confortoTxt  = conforto === true  ? '✓ Confortável'
                       : conforto === false ? '✗ Desconfortável'
                       : '--';
    const confortoColor = conforto === true  ? 'var(--acc-green, #3b82f6)'
                        : conforto === false ? '#ef4444'
                        : 'var(--text-muted)';

    const hasHistory = sensorHistory.length >= 2;
    const histLabel = hasHistory
        ? `${sensorHistory.length} amostras — últimas ${Math.round(sensorHistory.length / 2)}h`
        : 'Aguardando histórico do ESP32...';

    return (
        <div className="tab-content fade-in">
            {/* Alerta de conforto */}
            {conforto === false && (
                <div style={{
                    background: '#ef444422', border: '1px solid #ef4444',
                    borderRadius: '10px', padding: '10px 16px', marginBottom: '16px',
                    color: '#ef4444', fontWeight: 600, fontSize: '0.9rem'
                }}>
                    ⚠️ Ambiente desconfortável para o bebê — verifique temperatura e umidade
                </div>
            )}

            {/* Cards de leitura atual */}
            <div className="grid-cards">
                <div className="card">
                    <h3>Temperatura</h3>
                    {data.temp !== undefined
                        ? <><span className="val">{parseFloat(data.temp).toFixed(1)}</span><span className="unit">°C</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                </div>
                <div className="card">
                    <h3>Umidade</h3>
                    {data.umid !== undefined
                        ? <><span className="val">{parseFloat(data.umid).toFixed(0)}</span><span className="unit">%</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                </div>
                <div className="card">
                    <h3>Pressão</h3>
                    {data.pres !== undefined && data.pres > 0
                        ? <><span className="val">{parseFloat(data.pres).toFixed(1)}</span><span className="unit">hPa</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                    <div className="sub">Atmosférica</div>
                </div>
                <div className="card">
                    <h3>Conforto Térmico</h3>
                    {conforto !== undefined
                        ? <span className="val" style={{ fontSize: '1rem', color: confortoColor }}>{confortoTxt}</span>
                        : <Skeleton height="2.5rem" width="120px" style={{ marginTop: '8px' }} />}
                    <div className="sub">Para o bebê</div>
                </div>
            </div>

            {/* Seção histórico 24h */}
            <div style={{ marginTop: 28 }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
                    <h2 style={{ margin: 0, fontSize: '1rem', fontWeight: 700 }}>Histórico 24h</h2>
                    <span style={{ fontSize: '0.78rem', color: 'var(--text-muted)', background: 'var(--bg-card)',
                                   border: '1px solid var(--border-color)', borderRadius: 20, padding: '2px 10px' }}>
                        {histLabel}
                    </span>
                </div>

                <div style={{ display: 'flex', flexDirection: 'column', gap: 20 }}>

                    {/* Temperatura 24h */}
                    <div className="chart-card">
                        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
                            <h3 style={{ margin: 0 }}>Temperatura °C</h3>
                            <div style={{ display: 'flex', gap: 16 }}>
                                <span style={{ fontSize: '0.78rem', color: '#f59e0b' }}>● Temperatura</span>
                            </div>
                        </div>
                        <HistoryLineChart
                            samples={sensorHistory}
                            series={[{ key: 'temp', color: '#f59e0b' }]}
                            uptime_s={uptime_s}
                        />
                    </div>

                    {/* Umidade 24h */}
                    <div className="chart-card">
                        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
                            <h3 style={{ margin: 0 }}>Umidade %</h3>
                            <span style={{ fontSize: '0.78rem', color: '#3b82f6' }}>● Umidade</span>
                        </div>
                        <HistoryLineChart
                            samples={sensorHistory}
                            series={[{ key: 'umid', color: '#3b82f6' }]}
                            uptime_s={uptime_s}
                        />
                    </div>

                    {/* Confiança da IA 24h */}
                    <div className="chart-card">
                        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
                            <h3 style={{ margin: 0 }}>Confiança da IA %</h3>
                            <div style={{ display: 'flex', gap: 16 }}>
                                <span style={{ fontSize: '0.78rem', color: '#a78bfa' }}>● Trigger local</span>
                                <span style={{ fontSize: '0.78rem', color: '#f43f5e' }}>● Classificador</span>
                            </div>
                        </div>
                        <HistoryLineChart
                            samples={sensorHistory}
                            series={[
                                { key: 'conf', color: '#a78bfa' },
                                { key: 'rc',   color: '#f43f5e' },
                            ]}
                            uptime_s={uptime_s}
                        />
                    </div>

                    {/* Últimos 60 ciclos (polling rápido do browser) */}
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, marginTop: 8 }}>
                        <p style={{ margin: '0 0 4px 0', fontWeight: 700, fontSize: '0.9rem' }}>
                            Últimos 60 ciclos (tempo real)
                        </p>
                        <LineChartCard title="Temperatura °C" dataArray={history.temp} color="#f59e0b" />
                        <LineChartCard title="Umidade %"      dataArray={history.umid} color="#3b82f6" />
                        <LineChartCard title="Pressão hPa"    dataArray={history.pres} color="#a78bfa" />
                    </div>
                </div>
            </div>
        </div>
    );
}
