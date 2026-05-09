import { useState, useEffect } from 'react';
import { Activity, ThermometerSun, Brain, FileText, Settings, Info, ListTree, Moon, Sun, BarChart2, UploadCloud } from 'lucide-react';
import { Toaster } from 'react-hot-toast';
import { useDashboardData, formatUptime } from './hooks/useDashboardData';

// Placeholder components for views
import PerformanceView from './views/PerformanceView';
import SensorsView from './views/SensorsView';
import IAView from './views/IAView';
import LogsView from './views/LogsView';
import ConfigView from './views/ConfigView';
import AboutView from './views/AboutView';
import AnalyticsView from './views/AnalyticsView';
import OtaView from './views/OtaView';

const TABS = [
  { id: 'perf',  label: 'Performance', icon: Activity },
  { id: 'sens',  label: 'Sensores',    icon: ThermometerSun },
  { id: 'ia',    label: 'IA Status',   icon: Brain },
  { id: 'anal',  label: 'Análises',    icon: BarChart2 },
  { id: 'logs',  label: 'Logs',        icon: FileText },
  { id: 'conf',  label: 'Config',      icon: Settings },
  { id: 'ota',   label: 'Firmware',    icon: UploadCloud },
  { id: 'sobre', label: 'Sobre',       icon: Info },
  { id: 'chng',  label: 'Changelog',   icon: ListTree },
];

function App() {
  const [activeTab, setActiveTab] = useState('perf');
  const { data, history, sensorHistory, analytics, logs, config, fetchLogs, fetchConfig, fetchAnalytics, fetchSensorHistory } = useDashboardData();

  // Fetch logic on tab change
  useEffect(() => {
    if (activeTab === 'logs')  fetchLogs();
    if (activeTab === 'conf')  fetchConfig();
    if (activeTab === 'anal')  fetchAnalytics();
    if (activeTab === 'sens')  fetchSensorHistory();
  }, [activeTab, fetchLogs, fetchConfig, fetchAnalytics, fetchSensorHistory]);

  const [theme, setTheme] = useState(localStorage.getItem('theme') || 'light');
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    localStorage.setItem('theme', theme);
  }, [theme]);

  const renderContent = () => {
    switch (activeTab) {
      case 'perf': return <PerformanceView data={data} history={history} />;
      case 'sens': return <SensorsView data={data} history={history} sensorHistory={sensorHistory} />;
      case 'ia':   return <IAView data={data} />;
      case 'anal':  return <AnalyticsView analytics={analytics} sensorHistory={sensorHistory} uptime_s={data.uptime_s || 0} onRefresh={fetchAnalytics} />;
      case 'logs': return <LogsView logs={logs} onRefresh={fetchLogs} />;
      case 'conf': return <ConfigView config={config} onRefresh={fetchConfig} />;
      case 'ota':  return <OtaView />;
      case 'sobre': return <AboutView />;
      case 'chng': return (
        <div className="card">
          <h3 style={{ marginBottom: 12, color: 'var(--acc-blue)', fontSize: '1.1rem' }}>CrySense AI v2.0 - Mar/2026 (atual)</h3>
          <p style={{ color: 'var(--text-muted)', lineHeight: 1.6 }}>Arquitetura FreeRTOS multitarefa modernizada com React Vite para UI super responsiva. Adicionado suporte a novos endpoints, refino no layout, e nova identidade visual ciberfísica.</p>
        </div>
      );
      default: return null;
    }
  };

  return (
    <div className="app-container">
      <header className="header">
        <div className="header-brand">
          <img src="/logo.png" alt="CrySense AI Logo" className="header-logo" onError={(e) => { e.target.style.display = 'none'; }} />
          <h1 className="header-title">CrySense AI</h1>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
          <button
            onClick={() => setTheme(theme === 'light' ? 'dark' : 'light')}
            className="btn"
            style={{ padding: '8px', borderRadius: '50%' }}
            title="Alternar Tema"
          >
            {theme === 'light' ? <Moon size={18} /> : <Sun size={18} />}
          </button>
          <div className="header-status">
            <div className={`status-dot ${data.wifi_ok || data.uptime_s ? 'online' : ''}`}></div>
            <span id="hbTime">{data.uptime_s ? `Up: ${formatUptime(data.uptime_s)} | IP: ${data.ip || '--'}` : 'Aguardando...'}</span>
          </div>
        </div>
      </header>

      <nav className="nav-scroll">
        <div className="nav-tabs">
          {TABS.map((tab) => {
            const Icon = tab.icon;
            return (
              <button
                key={tab.id}
                className={`tab-btn ${activeTab === tab.id ? 'active' : ''}`}
                onClick={() => setActiveTab(tab.id)}
              >
                <Icon size={18} />
                {tab.label}
              </button>
            );
          })}
        </div>
      </nav>

      <main className="main-content pb-safe">
        {renderContent()}
      </main>

      <Toaster
        position="top-center"
        toastOptions={{
          style: {
            background: 'var(--bg-card)',
            color: 'var(--text-main)',
            border: '1px solid var(--border-color)',
            borderRadius: '12px',
            boxShadow: 'var(--shadow-card)'
          }
        }}
      />
    </div>
  );
}

export default App;
