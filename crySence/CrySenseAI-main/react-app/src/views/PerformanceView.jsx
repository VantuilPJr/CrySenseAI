import LineChartCard from '../components/LineChartCard';
import Skeleton from '../components/Skeleton';
import { formatUptime } from '../hooks/useDashboardData';

export default function PerformanceView({ data, history }) {
    return (
        <div className="tab-content fade-in">
            <div className="grid-cards">
                <div className="card">
                    <h3>CPU Core 0</h3>
                    <span className="val">{data.cpu0 ?? '--'}</span><span className="unit">%</span>
                    <div className="sub">Protocolo</div>
                </div>
                <div className="card">
                    <h3>CPU Core 1</h3>
                    <span className="val">{data.cpu1 ?? '--'}</span><span className="unit">%</span>
                    <div className="sub">IA + Áudio</div>
                </div>
                <div className="card">
                    <h3>Heap Livre</h3>
                    <span className="val">{data.heap ? (data.heap / 1024).toFixed(0) : '--'}</span><span className="unit">KB</span>
                    <div className="sub">Min: {data.heap_min ? (data.heap_min / 1024).toFixed(0) : '--'}</div>
                </div>
                <div className="card">
                    <h3>PSRAM Livre</h3>
                    <span className="val">{data.psram ? (data.psram / 1024).toFixed(0) : '--'}</span><span className="unit">KB</span>
                    <div className="sub">Total: --</div>
                </div>
                <div className="card">
                    <h3>Inferência</h3>
                    <span className="val">{data.inf_ms ?? '--'}</span><span className="unit">ms</span>
                    <div className="sub">Edge Impulse</div>
                </div>
                <div className="card">
                    <h3>Uptime</h3>
                    <span className="val" style={{ fontSize: '1.2rem' }}>{formatUptime(data.uptime_s)}</span>
                </div>
            </div>

            <div style={{ display: 'flex', flexDirection: 'column', gap: '24px', marginTop: '24px' }}>
                <LineChartCard
                    title="Heap Livre — últimos 60 ciclos (KB)"
                    dataArray={history.heap}
                    color="#00e5ff"
                />
                <LineChartCard
                    title="Tempo de Inferência IA — últimos 60 ciclos (ms)"
                    dataArray={history.inf}
                    color="#a855f7"
                    yAxisMin={0}
                />
            </div>
        </div>
    );
}
