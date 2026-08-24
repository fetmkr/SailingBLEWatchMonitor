import { defineConfig } from "vite";

// @ts-expect-error process is a nodejs global
const host = process.env.TAURI_DEV_HOST;

// https://vite.dev/config/
export default defineConfig(async () => ({

  // Vite options tailored for Tauri development and only applied in `tauri dev` or `tauri build`
  //
  // 1. prevent Vite from obscuring rust errors
  clearScreen: false,

  // 지도 부품은 웹 워커를 따로 들고 다닌다. Vite 가 미리 묶는 과정에서 그
  // 워커 파일을 흘려서 "maplibre-gl-worker.mjs 가 없다" 는 말이 떴다.
  // 미리 묶지 말라고 빼 둔다.
  optimizeDeps: { exclude: ["maplibre-gl"] },
  worker: { format: "es" },

  // 2. tauri expects a fixed port, fail if that port is not available
  server: {
    port: 1420,
    strictPort: true,
    host: host || false,
    hmr: host
      ? {
          protocol: "ws",
          host,
          port: 1421,
        }
      : undefined,
    watch: {
      // 3. tell Vite to ignore watching `src-tauri`
      ignored: ["**/src-tauri/**"],
    },
  },
}));
