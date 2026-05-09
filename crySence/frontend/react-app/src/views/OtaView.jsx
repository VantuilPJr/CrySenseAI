import { useState } from 'react';
import { Upload, RefreshCw } from 'lucide-react';
import toast from 'react-hot-toast';

export default function OtaView() {
    const [file, setFile] = useState(null);
    const [password, setPassword] = useState('');
    const [isUploading, setIsUploading] = useState(false);
    const [progress, setProgress] = useState(0);

    const handleFileChange = (e) => {
        if (e.target.files && e.target.files.length > 0) {
            setFile(e.target.files[0]);
        }
    };

    const handleUpload = () => {
        if (!file) {
            toast.error('Selecione um arquivo .bin do firmware.');
            return;
        }
        if (!password) {
            toast.error('Digite a senha para OTA.');
            return;
        }

        setIsUploading(true);
        setProgress(0);

        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/ota/upload', true);
        xhr.setRequestHeader('X-OTA-Password', password);

        xhr.upload.onprogress = (e) => {
            if (e.lengthComputable) {
                const percentComplete = Math.round((e.loaded / e.total) * 100);
                setProgress(percentComplete);
            }
        };

        xhr.onload = () => {
            if (xhr.status === 200) {
                toast.success('Update concluído! Reiniciando...');
                setProgress(100);
                setTimeout(() => {
                    window.location.reload();
                }, 3000);
            } else {
                toast.error('Falha no update: ' + xhr.responseText);
                setIsUploading(false);
            }
        };

        xhr.onerror = () => {
            toast.error('Erro de conexão durante o upload.');
            setIsUploading(false);
        };

        xhr.send(file);
    };

    return (
        <div className="tab-content fade-in" style={{ maxWidth: '600px', margin: '0 auto' }}>
            <div className="card">
                <h2 style={{ display: 'flex', alignItems: 'center', gap: '8px', color: '#ec4899', marginBottom: '24px' }}>
                    <Upload size={24} /> Atualização de Firmware (OTA)
                </h2>
                
                <p style={{ color: 'var(--text-muted)', marginBottom: '24px', lineHeight: 1.6 }}>
                    Selecione o arquivo de firmware (.bin) compilado pela Arduino IDE ou PlatformIO para atualizar o ESP32 remotamente.
                </p>

                <div className="form-group">
                    <label>Arquivo de Firmware (.bin)</label>
                    <input type="file" accept=".bin" onChange={handleFileChange} disabled={isUploading} style={{ background: 'var(--bg-base)' }} />
                </div>

                <div className="form-group" style={{ marginBottom: '32px' }}>
                    <label>Senha de Segurança</label>
                    <input 
                        type="password" 
                        value={password} 
                        onChange={(e) => setPassword(e.target.value)} 
                        placeholder="Senha configurada para OTA" 
                        disabled={isUploading}
                    />
                </div>

                {isUploading && (
                    <div style={{ marginBottom: '24px' }}>
                        <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '8px', fontSize: '0.9rem', color: 'var(--text-main)' }}>
                            <span>Progresso</span>
                            <span>{progress}%</span>
                        </div>
                        <div style={{ width: '100%', height: '12px', background: 'var(--bg-base)', borderRadius: '6px', overflow: 'hidden' }}>
                            <div style={{ height: '100%', width: `${progress}%`, background: 'var(--acc-blue)', transition: 'width 0.2s ease' }}></div>
                        </div>
                    </div>
                )}

                <button 
                    className="btn btn-primary" 
                    style={{ width: '100%', justifyContent: 'center', padding: '12px' }}
                    onClick={handleUpload}
                    disabled={isUploading || !file || !password}
                >
                    {isUploading ? (
                        <><RefreshCw className="spin" size={18} /> Enviando ({progress}%)</>
                    ) : (
                        <><Upload size={18} /> Iniciar Atualização</>
                    )}
                </button>
            </div>
            <style>{`
                .spin { animation: spin 1s linear infinite; }
                @keyframes spin { 100% { transform: rotate(360deg); } }
            `}</style>
        </div>
    );
}
