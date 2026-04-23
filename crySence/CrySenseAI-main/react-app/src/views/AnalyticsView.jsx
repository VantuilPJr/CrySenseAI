// AnalyticsView.jsx — Gráficos de análise histórica de episódios de choro
import { useEffect, useRef, useState } from 'react';
import { formatRelTime } from '../hooks/useDashboardData';

// Cores dos tipos
const COLORS = { COLICA: '#ef4444', FOME: '#f59e0b' };
const NOMES  = { COLICA: 'Cólica', FOME: 'Fome' };

// Mini-componente de barra de proporção
function PropBar({ value, total, color, label }) {
    const pct = total > 0 ? Math.round((value / total) * 100) : 0;
    return (
        <div style={{ display: 'flex', alignItems: 'center', gap: '10px', marginBottom: '8px' }}>
            <span style={{ width: 60, fontSize: '0.82rem', color: 'var(--text-muted)' }}>{label}</span>
            <div style={{ flex: 1, background: 'rgba(0,0,0,0.08)', borderRadius: 4, height: 10, overflow: 'hidden' }}>
                <div style={{ width: `${pct}%`, background: color, height: '100%', borderRadius: 4, transition: 'width 0.5s' }} />
            </div>
            <span style={{ width: 36, textAlign: 'right', fontSize: '0.82rem', color: 'var(--text-muted)' }}>{value}</span>
        </div>
    );
}

// Scatter chart nativo via canvas (sem dependência externa)
function ScatterCanvas({ eventos, uptime_s }) {
    const ref = useRef(null);

    useEffect(() => {
        const canvas = ref.current;
        if (!canvas || !eventos.length) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width, H = canvas.height;
        const PAD = { l: 40, r: 16, t: 16, b: 28 };
        const IW = W - PAD.l - PAD.r, IH = H - PAD.t - PAD.b;

        ctx.clearRect(0, 0, W, H);

        const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
        const gridColor = isDark ? '#30363d' : '#e2e8f0';
        const textColor = isDark ? '#8b949e' : '#64748b';
        const bg        = isDark ? '#161b22' : '#ffffff';

        ctx.fillStyle = bg;
        ctx.fillRect(0, 0, W, H);

        // Eixos
        const minTs = Math.min(...eventos.map(e => e.ts_s));
        const maxTs = Math.max(uptime_s || minTs + 60, minTs + 60);
        const xRange = maxTs - minTs || 1;

        // Grid horizontal (conf 0-100)
        for (let v = 0; v <= 100; v += 25) {
            const y = PAD.t + IH - (v / 100) * IH;
            ctx.strokeStyle = gridColor;
            ctx.lineWidth = 1;
            ctx.setLineDash([4, 4]);
            ctx.beginPath(); ctx.moveTo(PAD.l, y); ctx.lineTo(PAD.l + IW, y); ctx.stroke();
            ctx.fillStyle = textColor;
            ctx.font = '10px sans-serif';
            ctx.fillText(v + '%', 2, y + 4);
        }
        ctx.setLineDash([]);

        // Pontos
        eventos.forEach(e => {
            const x = PAD.l + ((e.ts_s - minTs) / xRange) * IW;
            const y = PAD.t + IH - (e.conf / 100) * IH;
            ctx.beginPath();
            ctx.arc(x, y, 7, 0, Math.PI * 2);
            ctx.fillStyle = COLORS[e.tipo] || '#94a3b8';
            ctx.fill();
            ctx.strokeStyle = bg;
            ctx.lineWidth = 1.5;
            ctx.stroke();
        });

        // Eixo X — labels de tempo relativo (min)
        ctx.fillStyle = textColor;
        ctx.font = '9px sans-serif';
        for (let i = 0; i <= 4; i++) {
            const ts = minTs + (i / 4) * xRange;
            const x  = PAD.l + (i / 4) * IW;
            const min = Math.round((ts - minTs) / 60);
            ctx.fillText(min + 'min', x - 10, H - 4);
        }
    }, [eventos, uptime_s]);

    return <canvas ref={ref} width={600} height={180} style={{ width: '100%', height: 180, borderRadius: 8 }} />;
}

export default function AnalyticsView({ analytics, uptime_s, onRefresh }) {
    const [refreshing, setRefreshing] = useState(false);
    const evts   = analytics.eventos || [];
    const resumo = analytics.resumo  || {};
    const total  = resumo.total  || 0;
    const nColica = resumo.colica || 0;
    const nFome   = resumo.fome   || 0;

    async function refresh() {
        setRefreshing(true);
        await onRefresh();
        setRefreshing(false);
    }

    return (
        <div className="tab-content fade-in">

            {/* Botão atualizar */}
            <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 16 }}>
                <button className="btn" onClick={refresh} disabled={refreshing}>
                    {refreshing ? 'Atualizando...' : '↻ Atualizar'}
                </button>
            </div>

            {/* Resumo contadores */}
            <div className="grid-cards" style={{ marginBottom: 20 }}>
                <div className="card" style={{ borderTop: `3px solid #ef4444` }}>
                    <h3>Episódios de Cólica</h3>
                    <span className="val" style={{ color: '#ef4444' }}>{nColica}</span>
                </div>
                <div className="card" style={{ borderTop: `3px solid #f59e0b` }}>
                    <h3>Episódios de Fome</h3>
                    <span className="val" style={{ color: '#f59e0b' }}>{nFome}</span>
                </div>
                <div className="card">
                    <h3>Total de episódios</h3>
                    <span className="val">{total}</span>
                    <div className="sub">Desde o boot</div>
                </div>
            </div>

            {total === 0 ? (
                <div className="chart-card" style={{ textAlign: 'center', color: 'var(--text-muted)', padding: 40 }}>
                    <p style={{ fontSize: '2rem' }}>😴</p>
                    <p style={{ marginTop: 12 }}>Nenhum episódio de choro registrado ainda.</p>
                    <p style={{ fontSize: '0.82rem', marginTop: 4 }}>Os dados aparecem assim que o bebê chorar e o sistema confirmar.</p>
                </div>
            ) : (<>

                {/* Scatter timeline */}
                <div className="chart-card" style={{ marginBottom: 20 }}>
                    <h3 style={{ marginBottom: 12 }}>Episódios ao longo do tempo  <span style={{ fontSize: '0.8rem', fontWeight: 400, color: 'var(--text-muted)' }}>(eixo Y = confiança %)</span></h3>
                    <div style={{ display: 'flex', gap: 16, marginBottom: 12 }}>
                        {Object.entries(COLORS).map(([k, c]) => (
                            <div key={k} style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                                <div style={{ width: 10, height: 10, borderRadius: '50%', background: c }} />
                                {NOMES[k]}
                            </div>
                        ))}
                    </div>
                    <ScatterCanvas eventos={evts} uptime_s={uptime_s} />
                </div>

                {/* Proporção + Últimos eventos lado a lado */}
                <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 16, marginBottom: 20 }}>

                    {/* Proporção por tipo */}
                    <div className="chart-card">
                        <h3 style={{ marginBottom: 16 }}>Distribuição por tipo</h3>
                        <PropBar value={nColica} total={total} color="#ef4444" label="Cólica" />
                        <PropBar value={nFome}   total={total} color="#f59e0b" label="Fome"   />
                        <div style={{ marginTop: 16, fontSize: '0.82rem', color: 'var(--text-muted)' }}>
                            {total} episódio{total !== 1 ? 's' : ''} registrado{total !== 1 ? 's' : ''}
                        </div>
                    </div>

                    {/* Picos de confiança */}
                    <div className="chart-card">
                        <h3 style={{ marginBottom: 16 }}>Picos de confiança (top 5)</h3>
                        {[...evts].sort((a, b) => b.conf - a.conf).slice(0, 5).map((e, i) => (
                            <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 8 }}>
                                <span style={{ width: 8, height: 8, borderRadius: '50%', background: COLORS[e.tipo], display: 'inline-block', flexShrink: 0 }} />
                                <span style={{ fontSize: '0.82rem', flex: 1, color: 'var(--text-muted)' }}>{NOMES[e.tipo]}</span>
                                <span style={{ fontWeight: 700, color: COLORS[e.tipo], fontSize: '0.9rem' }}>{e.conf}%</span>
                            </div>
                        ))}
                    </div>
                </div>

                {/* Tabela de histórico completo */}
                <div className="chart-card">
                    <h3 style={{ marginBottom: 12 }}>Histórico de episódios</h3>
                    <div style={{ overflowX: 'auto' }}>
                        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: '0.85rem' }}>
                            <thead>
                                <tr style={{ color: 'var(--text-muted)', textAlign: 'left' }}>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>#</th>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>Tempo</th>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>Tipo</th>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>Confiança</th>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>Temp.</th>
                                    <th style={{ padding: '6px 12px', borderBottom: '1px solid var(--border-color)' }}>Umid.</th>
                                </tr>
                            </thead>
                            <tbody>
                                {[...evts].reverse().map((e, i) => (
                                    <tr key={i} style={{ borderBottom: '1px solid var(--border-color)' }}>
                                        <td style={{ padding: '8px 12px', color: 'var(--text-muted)', fontSize: '0.75rem' }}>{evts.length - i}</td>
                                        <td style={{ padding: '8px 12px', color: 'var(--text-muted)', fontSize: '0.8rem' }}>
                                            {formatRelTime(e.ts_s, uptime_s)}
                                        </td>
                                        <td style={{ padding: '8px 12px' }}>
                                            <span style={{
                                                padding: '2px 10px', borderRadius: 12, fontSize: '0.78rem',
                                                background: (COLORS[e.tipo] || '#94a3b8') + '22',
                                                color: COLORS[e.tipo] || '#94a3b8',
                                                border: `1px solid ${COLORS[e.tipo] || '#94a3b8'}`,
                                                fontWeight: 700,
                                            }}>
                                                {NOMES[e.tipo] || e.tipo}
                                            </span>
                                        </td>
                                        <td style={{ padding: '8px 12px', fontWeight: 700, color: COLORS[e.tipo] || 'var(--text-main)' }}>{e.conf}%</td>
                                        <td style={{ padding: '8px 12px', color: 'var(--text-muted)' }}>{e.temp != null ? e.temp.toFixed(1) + '°C' : '--'}</td>
                                        <td style={{ padding: '8px 12px', color: 'var(--text-muted)' }}>{e.umid != null ? Math.round(e.umid) + '%' : '--'}</td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                    <p style={{ fontSize: '0.75rem', color: 'var(--text-muted)', marginTop: 8 }}>
                        * Histórico em RAM — máx. 48 episódios. Reiniciar o ESP32 apaga os dados.
                    </p>
                </div>

            </>)}
        </div>
    );
}
