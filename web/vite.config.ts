import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

export default defineConfig({
  base: "./",
  plugins: [preact()],
  build: {
    cssCodeSplit: false,
    sourcemap: false,
    target: "es2020",
  },
  test: {
    environment: "node",
  },
});
