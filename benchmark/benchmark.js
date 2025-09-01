import createQuoridorAIModule from '../public/ai/ai.js';

// --- Configuration ---
const BENCHMARK_DEPTH = 4;

function createMockGameState() {
    return {
        boardSize: 9,
        pawnPositions: { p1: { row: 7, col: 4 }, p3: { row: 1, col: 4 } },
        wallsLeft: { p1: 9, p3: 10 },
        placedWalls: [{ row: 6, col: 5, orientation: 'horizontal' }],
        playerTurn: 'p1',
        activePlayerIds: ['p1', 'p3'],
        playerTurnIndex: 0, status: 'active', winner: ''
    };
}

function createMockPlayers() {
    return [{ id: 'p1' }, { id: 'p3' }];
}

async function run() {
    console.log('Loading AI WebAssembly module...');
    const aiModule = await createQuoridorAIModule();
    console.log(`Module loaded. Starting ablation study with depth: ${BENCHMARK_DEPTH}...\n`);

    const jsState = createMockGameState();
    const jsPlayers = createMockPlayers();

    try {
        const results = aiModule.runAblationBenchmark(jsState, jsPlayers, BENCHMARK_DEPTH);

        // Convert Emscripten::val to a native JS array of objects
        const benchmarkData = [];
        for (let i = 0; i < results.length; i++) {
            benchmarkData.push({
                Configuration: results[i].name,
                'Time (ms)': results[i].timeMs,
                Score: results[i].score,
            });
        }
        
        // Calculate speedup relative to the vanilla implementation
        const baselineTime = benchmarkData.find(r => r.Configuration === 'Vanilla NegaMax (None)')['Time (ms)'];
        
        const formattedData = benchmarkData.map(data => ({
            ...data,
            Speedup: baselineTime > 0 && data['Time (ms)'] > 0 
                ? `${(Number(baselineTime) / Number(data['Time (ms)'])).toFixed(2)}x` 
                : 'N/A'
                    })).sort((a, b) => Number(a['Time (ms)']) - Number(b['Time (ms)']));

        console.log('--- Ablation Benchmark Results ---');
        console.table(formattedData);

    } catch (error) {
        console.error("An error occurred during benchmark execution:", error);
    }
}

run();