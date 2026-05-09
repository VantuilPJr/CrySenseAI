// IAView.jsx — Pipeline dual-IA com visualização das duas etapas
export default function IAView({ data }) {
    const label = (data.label || 'noise').toLowerCase();
    const scores = data.scores || {};
    const estado = data.estado || '';
    const pipeline = (data.pipeline || 'idle').toLowerCase();
    const resultLabel = (data.result_label || '').toLowerCase();
    const resultLabelPt = data.result_label_pt || data.result_label || '--';
    const remoteErr = Number(data.remote_err || 0);
    const remoteLatencyMs = Number(data.remote_latency_ms || 0);
    const allowedScoreKeys = ['colic', 'hunger', 'noise'];

    const isCrise = estado === 'crise';
    const isTriggerCry = label === 'choro' || label === 'cry';
    const hasRemoteResult = pipeline === 'result' && (resultLabel === 'colic' || resultLabel === 'hunger');

    const finalLabel = hasRemoteResult ? resultLabel : (isTriggerCry ? 'choro' : 'noise');
    const finalLabelPt = hasRemoteResult ? resultLabelPt : (isTriggerCry ? 'Analisando...' : 'Ruído');
    const finalConf = hasRemoteResult ? (data.result_conf ?? null) : (isTriggerCry ? (data.confianca ?? null) : null);

    // Cores e nomes das classes
    const colors = { colic: '#ef4444', hunger: '#f59e0b', choro: '#a855f7', noise: '#94a3b8' };
    const nomes = { colic: 'Cólica', hunger: 'Fome', noise: 'Ruído' };

    const pipelineInfo = {
        idle:           { txt: '— (aguardando choro)',      bg: 'var(--bg-input, #f1f5f9)', border: 'var(--border-color)', color: 'var(--text-muted)' },
        recording:      { txt: '🎙️ Gravando áudio (6s)',     bg: '#3b82f622', border: '#3b82f6', color: '#3b82f6' },
        uploading:      { txt: '📤 Enviando para servidor',  bg: '#06b6d422', border: '#06b6d4', color: '#0891b2' },
        waiting_result: { txt: '🧠 Analisando no notebook',  bg: '#a855f722', border: '#a855f7', color: '#a855f7' },
        error:          { txt: '⚠️ Falha de comunicação',    bg: '#ef444422', border: '#ef4444', color: '#ef4444' },
    };
    const t2 = hasRemoteResult
        ? {
            txt: resultLabelPt,
            bg: resultLabel === 'colic' ? '#ef444422' : '#f59e0b22',
            border: resultLabel === 'colic' ? '#ef4444' : '#f59e0b',
            color: resultLabel === 'colic' ? '#ef4444' : '#f59e0b',
        }
        : (pipelineInfo[pipeline] || pipelineInfo.idle);

    // Cor do alerta geral
    const alertaBg = { COLICA: '#ef444420', FOME: '#f59e0b20' };
    const alertaBord = { COLICA: '#ef4444', FOME: '#f59e0b' };
    const alertaNome = { COLICA: '😭 Cólica detectada!', FOME: '🍼 Bebê com fome!' };
    const ua = (data.ultimo_alerta || '').toUpperCase();

    const classBadgeStyle = {
        display: 'inline-flex',
        padding: '6px 14px',
        borderRadius: '20px',
        fontSize: '0.8rem',
        fontWeight: 700,
        textTransform: 'uppercase',
        letterSpacing: '0.05em',
        border: `1px solid ${t2.border}`,
        background: t2.bg,
        color: t2.color,
    };

    return (
        <div className="tab-content fade-in">

            {/* Banner de alerta ativo */}
            {isCrise && ua && (
                <div style={{
                    background: alertaBg[ua] || '#ef444422',
                    border: `1px solid ${alertaBord[ua] || '#ef4444'}`,
                    borderLeft: `4px solid ${alertaBord[ua] || '#ef4444'}`,
                    borderRadius: '10px', padding: '12px 18px', marginBottom: '20px',
                    color: alertaBord[ua] || '#ef4444', fontWeight: 700, fontSize: '1rem',
                    display: 'flex', alignItems: 'center', justifyContent: 'space-between'
                }}>
                    <span>{alertaNome[ua] || ua}</span>
                    <span style={{ fontWeight: 400, fontSize: '0.85rem' }}>
                        Confiança: {finalConf ?? '--'}%
                        {data.audio ? '  🔊 Áudio ativo' : ''}
                    </span>
                </div>
            )}

            {/* Pipeline dual-IA */}
            <div className="chart-card" style={{ marginBottom: '20px' }}>
                <h3 style={{ marginBottom: '16px' }}>Pipeline de Detecção (2 etapas)</h3>
                <div style={{ display: 'flex', alignItems: 'center', gap: '12px', flexWrap: 'wrap' }}>

                    {/* Etapa 1 — Trigger */}
                    <div style={{ textAlign: 'center' }}>
                        <div style={{ fontSize: '0.7rem', color: 'var(--text-muted)', marginBottom: '6px', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                            Etapa 1 — Trigger
                        </div>
                        <span style={{
                            padding: '6px 16px', borderRadius: '20px', fontWeight: 700,
                            fontSize: '0.85rem', border: '1px solid',
                            background: isTriggerCry ? '#a855f722' : 'var(--bg-input, #f1f5f9)',
                            borderColor: isTriggerCry ? '#a855f7' : 'var(--border-color)',
                            color: isTriggerCry ? '#a855f7' : 'var(--text-muted)',
                        }}>
                            {isTriggerCry ? '🔊 Choro detectado' : '🔇 Ruído / Silêncio'}
                        </span>
                    </div>

                    <div style={{ fontSize: '1.4rem', color: 'var(--text-muted)' }}>→</div>

                    {/* Etapa 2 — Classificador */}
                    <div style={{ textAlign: 'center' }}>
                        <div style={{ fontSize: '0.7rem', color: 'var(--text-muted)', marginBottom: '6px', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                            Etapa 2 — Classificador
                        </div>
                        <span style={{
                            padding: '6px 16px', borderRadius: '20px', fontWeight: 700,
                            fontSize: '0.85rem', border: `1px solid ${t2.border}`,
                            background: t2.bg, color: t2.color,
                        }}>{t2.txt}</span>
                    </div>

                    {/* Confiança final */}
                    <div style={{ marginLeft: 'auto', textAlign: 'right' }}>
                        <div style={{ fontSize: '0.7rem', color: 'var(--text-muted)', marginBottom: '4px', textTransform: 'uppercase' }}>Confiança</div>
                        <span style={{ fontSize: '1.8rem', fontWeight: 700, color: colors[finalLabel] || 'var(--text-muted)' }}>
                            {finalConf !== null ? `${finalConf}%` : '—'}
                        </span>
                        <div style={{ fontSize: '0.78rem', color: 'var(--text-muted)', marginTop: '3px' }}>
                            {remoteLatencyMs > 0 ? `${remoteLatencyMs}ms` : '\u00A0'}
                        </div>
                    </div>
                </div>
                {pipeline === 'error' && (
                    <div style={{ marginTop: '12px', fontSize: '0.82rem', color: 'var(--text-muted)' }}>
                        Erros remotos acumulados: {remoteErr}
                    </div>
                )}
            </div>

            {/* Cards resumo */}
            <div className="grid-cards" style={{ marginBottom: '20px' }}>
                <div className="card">
                    <h3>Classificação</h3>
                    <span style={classBadgeStyle}>{finalLabelPt}</span>
                </div>
                <div className="card">
                    <h3>Estado do sistema</h3>
                    <span className="val" style={{ fontSize: '1.2rem', textTransform: 'capitalize' }}>{estado || '--'}</span>
                    <div className="sub">Pipeline: {pipeline.replace('_', ' ')}</div>
                </div>
                <div className="card">
                    <h3>Último alerta</h3>
                    <span className="val" style={{ fontSize: '1.1rem', color: alertaBord[ua] || 'var(--text-muted)' }}>
                        {data.ultimo_alerta || 'Nenhum'}
                    </span>
                </div>
                <div className="card">
                    <h3>Áudio</h3>
                    <span className="val" style={{ fontSize: '1.1rem' }}>
                        {data.audio ? '🔊 Ativo' : '🔇 Inativo'}
                    </span>
                </div>
            </div>

            {/* Barras de probabilidade (escondidas durante Crise para evitar poluição visual) */}
            {!isCrise && (
                <div className="chart-card">
                    <h3 style={{ marginBottom: '16px' }}>Distribuição de probabilidade (última inferência)</h3>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: '12px' }}>
                        {Object.entries(scores)
                            .filter(([k, v]) => allowedScoreKeys.includes(k) && (isCrise ? k !== 'noise' : v > 0))
                            .sort((a, b) => b[1] - a[1])
                            .map(([k, v]) => {
                                const pct = (v * 100).toFixed(0);
                                let nome = nomes[k] || k;
                                if (!isCrise && k === 'noise' && pct > 0 && !hasRemoteResult && isTriggerCry) {
                                    nome = 'Analisando...';
                                }
                                return (
                                    <div key={k} style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
                                        <label style={{ width: '90px', margin: 0, fontSize: '0.85rem', color: 'var(--text-muted)' }}>{nome}</label>
                                        <div style={{ flex: 1, background: 'rgba(0,0,0,0.08)', borderRadius: '6px', height: '10px', overflow: 'hidden' }}>
                                            <div style={{ width: `${pct}%`, background: colors[k] || '#94a3b8', height: '100%', borderRadius: '6px', transition: 'width 0.5s ease-out' }} />
                                        </div>
                                        <span style={{ fontSize: '0.82rem', color: 'var(--text-muted)', width: '36px', textAlign: 'right' }}>{pct}%</span>
                                    </div>
                                );
                            })}
                        {Object.keys(scores).length === 0 && (
                            <p style={{ color: 'var(--text-muted)' }}>Nenhuma inferência recente.</p>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
}
