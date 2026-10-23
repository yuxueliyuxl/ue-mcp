import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["tests/**/*.test.ts"],
    // Spawned-process suites with their own parallel config (vitest.integration.config.ts).
    exclude: ["**/node_modules/**", "tests/integration/**"],
    setupFiles: ["tests/setup/isolate-user-state.ts"],
    testTimeout: 60_000,
    hookTimeout: 60_000,
    reporters: ["verbose"],
    fileParallelism: false,
    sequence: { concurrent: false },
    pool: "forks",
    poolOptions: { forks: { singleFork: true } },
  },
});
