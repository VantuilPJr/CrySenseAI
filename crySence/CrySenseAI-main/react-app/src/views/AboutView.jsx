import { GraduationCap, BookOpen, User } from 'lucide-react';

export default function AboutView() {
    return (
        <div className="tab-content fade-in" style={{ maxWidth: '600px' }}>
            <div className="chart-card" style={{ padding: '32px', textAlign: 'center' }}>

                <div style={{ marginBottom: '24px' }}>
                    <img
                        src="/logo.png"
                        alt="CrySense AI"
                        style={{ width: '120px', height: '120px', objectFit: 'contain', margin: '0 auto 16px', display: 'block' }}
                        onError={(e) => { e.target.style.display = 'none'; }}
                    />
                    <h2 style={{ fontSize: '1.5rem', fontWeight: 700, background: 'linear-gradient(to right, #60a5fa, #3b82f6)', WebkitBackgroundClip: 'text', color: 'transparent' }}>
                        CrySense AI
                    </h2>
                    <p style={{ color: 'var(--text-muted)', marginTop: '8px' }}>v2.0 — Sistema Ciberfísico Preditive Edge AI</p>
                </div>

                <div style={{
                    background: 'rgba(59, 130, 246, 0.05)',
                    border: '1px solid rgba(59, 130, 246, 0.2)',
                    borderRadius: '12px',
                    padding: '24px',
                    textAlign: 'left',
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '16px'
                }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
                        <div style={{ background: 'var(--bg-card)', padding: '10px', borderRadius: '8px' }}>
                            <GraduationCap size={20} color="var(--acc-blue-light)" />
                        </div>
                        <div>
                            <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>Universidade</div>
                            <div style={{ fontWeight: 500 }}>PUCPR</div>
                        </div>
                    </div>

                    <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
                        <div style={{ background: 'var(--bg-card)', padding: '10px', borderRadius: '8px' }}>
                            <BookOpen size={20} color="#f59e0b" />
                        </div>
                        <div>
                            <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>Disciplina</div>
                            <div style={{ fontWeight: 500 }}>Performance de Sistemas Ciberfísicos</div>
                        </div>
                    </div>

                    <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
                        <div style={{ background: 'var(--bg-card)', padding: '10px', borderRadius: '8px' }}>
                            <User size={20} color="#10b981" />
                        </div>
                        <div>
                            <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>Autor</div>
                            <div style={{ fontWeight: 500 }}>vantuil.plaster</div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    );
}
