import LineChartCard from '../components/LineChartCard';
import Skeleton from '../components/Skeleton';

export default function SensorsView({ data, history }) {
    const conforto = data.conforto_ok;
    const confortoTxt  = conforto === true  ? '✓ Confortável'
                       : conforto === false ? '✗ Desconfortável'
                       : '--';
    const confortoColor = conforto === true  ? 'var(--acc-green, #3b82f6)'
                        : conforto === false ? '#ef4444'
                        : 'var(--text-muted)';

    return (
        <div className="tab-content fade-in">
            {/* Linha de alertas de conforto */}
            {conforto === false && (
                <div style={{
                    background: '#ef444422', border: '1px solid #ef4444',
                    borderRadius: '10px', padding: '10px 16px', marginBottom: '16px',
                    color: '#ef4444', fontWeight: 600, fontSize: '0.9rem'
                }}>
                    ⚠️ Ambiente desconfortável para o bebê — verifique temperatura e umidade
                </div>
            )}

            <div className="grid-cards">
                {/* Temperatura */}
                <div className="card">
                    <h3>Temperatura</h3>
                    {data.temp !== undefined
                        ? <><span className="val">{parseFloat(data.temp).toFixed(1)}</span><span className="unit">°C</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                </div>

                {/* Umidade */}
                <div className="card">
                    <h3>Umidade</h3>
                    {data.umid !== undefined
                        ? <><span className="val">{parseFloat(data.umid).toFixed(0)}</span><span className="unit">%</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                </div>

                {/* Pressão — agora com dado real da API */}
                <div className="card">
                    <h3>Pressão</h3>
                    {data.pres !== undefined && data.pres > 0
                        ? <><span className="val">{parseFloat(data.pres).toFixed(1)}</span><span className="unit">hPa</span></>
                        : <Skeleton height="2.5rem" width="80px" style={{ marginTop: '8px' }} />}
                    <div className="sub">Atmosférica</div>
                </div>

                {/* Conforto Térmico — agora com lógica real */}
                <div className="card">
                    <h3>Conforto Térmico</h3>
                    {conforto !== undefined
                        ? <span className="val" style={{ fontSize: '1rem', color: confortoColor }}>{confortoTxt}</span>
                        : <Skeleton height="2.5rem" width="120px" style={{ marginTop: '8px' }} />}
                    <div className="sub">Para o bebê</div>
                </div>
            </div>

            <div style={{ display: 'flex', flexDirection: 'column', gap: '24px', marginTop: '24px' }}>
                <LineChartCard
                    title="Temperatura °C — últimos 60 ciclos"
                    dataArray={history.temp}
                    color="#f59e0b"
                />
                <LineChartCard
                    title="Umidade % — últimos 60 ciclos"
                    dataArray={history.umid}
                    color="#3b82f6"
                />
                <LineChartCard
                    title="Pressão hPa — últimos 60 ciclos"
                    dataArray={history.pres}
                    color="#a78bfa"
                />
            </div>
        </div>
    );
}
