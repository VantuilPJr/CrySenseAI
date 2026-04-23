export default function Skeleton({ width = '100%', height = '1rem', className = '', style = {} }) {
    return (
        <div
            className={`skeleton-base ${className}`}
            style={{
                width,
                height,
                borderRadius: '6px',
                ...style
            }}
        />
    );
}
