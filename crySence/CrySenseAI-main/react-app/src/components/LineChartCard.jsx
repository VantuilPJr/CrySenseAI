import {
    Chart as ChartJS,
    CategoryScale,
    LinearScale,
    PointElement,
    LineElement,
    Title,
    Tooltip,
    Filler,
    Legend,
} from 'chart.js';
import { Line } from 'react-chartjs-2';

ChartJS.register(
    CategoryScale,
    LinearScale,
    PointElement,
    LineElement,
    Title,
    Tooltip,
    Filler,
    Legend
);

export default function LineChartCard({ title, dataArray, color, yAxisMin }) {
    const options = {
        responsive: true,
        maintainAspectRatio: false,
        plugins: {
            legend: { display: false },
            tooltip: {
                backgroundColor: 'rgba(15, 23, 42, 0.9)',
                titleColor: '#fff',
                bodyColor: '#e2e8f0',
                borderColor: 'rgba(255, 255, 255, 0.1)',
                borderWidth: 1,
                padding: 12,
                displayColors: false,
                callbacks: {
                    title: () => null,
                    label: (context) => `${context.dataset.label}: ${context.parsed.y}`
                }
            }
        },
        interaction: {
            mode: 'index',
            intersect: false,
        },
        animation: {
            duration: 800,
            easing: 'easeOutQuart'
        },
        scales: {
            x: { display: false },
            y: {
                min: yAxisMin,
                grid: { color: 'rgba(128, 128, 128, 0.1)' },
                ticks: { color: 'rgba(128, 128, 128, 0.6)' }
            }
        },
        elements: {
            point: { radius: 0 }
        }
    };

    const data = {
        labels: dataArray.map((_, i) => i),
        datasets: [
            {
                fill: true,
                label: title,
                data: dataArray,
                borderColor: color,
                backgroundColor: `${color}20`,
                tension: 0.1,
                borderWidth: 2,
            },
        ],
    };

    return (
        <div className="chart-card">
            <h3>{title}</h3>
            <div style={{ height: '200px' }}>
                <Line options={options} data={data} />
            </div>
        </div>
    );
}
