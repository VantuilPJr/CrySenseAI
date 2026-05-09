import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],

  build: {
    // Destino direto para a pasta data/ do projeto Arduino
    outDir: path.resolve(__dirname, '../../data'),
    emptyOutDir: true, // Limpa a pasta antes de cada build — evita acúmulo de arquivos antigos!

    rollupOptions: {
      output: {
        // Nomes FIXOS (sem hash) — o arquivo antigo é sempre sobrescrito, não acumulado
        entryFileNames: 'assets/index.js',
        chunkFileNames: 'assets/index.js',
        assetFileNames: 'assets/index.[ext]',
      },
    },

    // Vite v8 usa oxc como minificador padrão automaticamente
  },
})
