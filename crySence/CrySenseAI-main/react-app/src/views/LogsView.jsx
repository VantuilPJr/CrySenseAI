import { useState, useMemo } from 'react';
import { RefreshCw, Download, Trash2 } from 'lucide-react';
import toast from 'react-hot-toast';

export default function LogsView({ logs, onRefresh }) {
    const [filter, setFilter] = useState('');

    const filteredLogs = useMemo(() => {
        let result = logs || [];
        if (filter) {
            result = result.filter(e => e.l === filter);
        }
        return result.slice(-200).reverse();
    }, [logs, filter]);

    const limparLogs = async () => {
        toast((t) => (
            <div>
                <p style={{ marginBottom: '8px' }}>Limpar todos os logs permanentes do dispositivo?</p>
                <div style={{ display: 'flex', gap: '8px', justifyContent: 'flex-end' }}>
                    <button className="btn btn-danger" onClick={async () => {
                        toast.dismiss(t.id);
                        const toastId = toast.loading('Limpando logs...');
                        try {
                            const res = await fetch('/api/logs/clear', { method: 'POST' });
                            if (res.ok) {
                                toast.success('Logs removidos.', { id: toastId });
                                onRefresh?.();
                            } else {
                                toast.error('Falha ao limpar logs.', { id: toastId });
                            }
                        } catch (err) {
                            console.error(err);
                            toast.error('Erro de rede ao limpar logs.', { id: toastId });
                        }
                    }}>Confirmar</button>
                    <button className="btn" onClick={() => toast.dismiss(t.id)}>Cancelar</button>
                </div>
            </div>
        ), { duration: Infinity });
    };

    const exportarLogs = async () => {
        const toastId = toast.loading('Gerando CSV...');
        try {
            const res = await fetch('/api/logs/csv');
            if (!res.ok) {
                toast.error('Falha ao exportar CSV.', { id: toastId });
                return;
            }

            const csv = await res.text();
            const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });
            const url = window.URL.createObjectURL(blob);
            const link = document.createElement('a');
            link.href = url;
            link.download = `crysense-logs-${Date.now()}.csv`;
            document.body.appendChild(link);
            link.click();
            link.remove();
            window.URL.revokeObjectURL(url);
            toast.success('CSV exportado.', { id: toastId });
        } catch (err) {
            console.error(err);
            toast.error('Erro de rede ao exportar CSV.', { id: toastId });
        }
    };

    const tagColor = (level) => {
        switch (level) {
            case 'I': return 'var(--acc-blue-light)';
            case 'W': return 'var(--warn)';
            case 'E': return 'var(--err)';
            case 'A': return 'var(--err)';
            default: return 'var(--text-main)';
        }
    };

    return (
        <div className="tab-content fade-in">
            <div style={{ display: 'flex', gap: '12px', marginBottom: '20px', flexWrap: 'wrap', alignItems: 'center' }}>
                <select value={filter} onChange={e => setFilter(e.target.value)} style={{ width: '140px', margin: 0 }}>
                    <option value="">Todos os Níveis</option>
                    <option value="I">Info</option>
                    <option value="W">Warning</option>
                    <option value="E">Erro</option>
                    <option value="A">Alerta</option>
                </select>

                <button className="btn" onClick={onRefresh}>
                    <RefreshCw size={16} /> Atualizar
                </button>
                <button className="btn" onClick={exportarLogs}>
                    <Download size={16} /> Exportar CSV
                </button>
                <button className="btn btn-danger" onClick={limparLogs}>
                    <Trash2 size={16} /> Limpar
                </button>
                <span style={{ fontSize: '0.85rem', color: 'var(--text-muted)', marginLeft: '8px' }}>
                    {filteredLogs.length} linhas
                </span>
            </div>

            <div className="chart-card" style={{ padding: 0, overflow: 'hidden' }}>
                <div style={{ overflowX: 'auto', maxHeight: '600px' }}>
                    <table>
                        <thead style={{ position: 'sticky', top: 0, background: 'var(--bg-card)', zIndex: 1 }}>
                            <tr>
                                <th style={{ width: '120px' }}>Tempo (ms)</th>
                                <th style={{ width: '80px' }}>Nível</th>
                                <th>Mensagem</th>
                            </tr>
                        </thead>
                        <tbody>
                            {filteredLogs.map((log, idx) => (
                                <tr key={idx}>
                                    <td style={{ color: 'var(--text-muted)' }}>{log.ts}</td>
                                    <td style={{ color: tagColor(log.l), fontWeight: 600 }}>{log.l}</td>
                                    <td style={{ fontFamily: 'monospace' }}>{log.m}</td>
                                </tr>
                            ))}
                            {filteredLogs.length === 0 && (
                                <tr>
                                    <td colSpan="3" style={{ textAlign: 'center', padding: '32px', color: 'var(--text-muted)' }}>
                                        Nenhum log encontrado para esse filtro.
                                    </td>
                                </tr>
                            )}
                        </tbody>
                    </table>
                </div>
            </div>
        </div>
    );
}
