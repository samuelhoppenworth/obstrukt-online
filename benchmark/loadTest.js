import { io } from 'socket.io-client';

// --- Configuration ---
const SERVER_URL = 'https://obstrukt.vercel.app';
const NUM_ROOMS_TO_TEST = 10; 
const PLAYERS_PER_ROOM = 2;
const GAME_DURATION_MS = 45000; 
const MOVE_INTERVAL_MS = 2000; 

let roomsCompleted = 0;
let roomsFailed = 0;

/**
 * Simulates a single game session between two clients.
 * Returns a Promise that resolves on successful game completion or rejects on error/timeout.
 */
function createGameSession(roomIndex) {
    return new Promise((resolve, reject) => {
        let gameStarted = false;
        let gameFinished = false;
        const clients = [];
        const playerRoles = {}; // Maps socket ID to player role (p1, p3)
        let activeTurn = null;

        const sessionTimeout = setTimeout(() => {
            if (!gameFinished) {
                cleanupAndReject(`Room ${roomIndex} timed out after ${GAME_DURATION_MS / 1000}s.`);
            }
        }, GAME_DURATION_MS + 2000); // Add a small buffer

        const cleanupAndResolve = (message) => {
            if (gameFinished) return;
            gameFinished = true;
            clearTimeout(sessionTimeout);
            clients.forEach(c => c.disconnect());
            console.log(message);
            roomsCompleted++;
            resolve();
        };

        const cleanupAndReject = (error) => {
            if (gameFinished) return;
            gameFinished = true;
            clearTimeout(sessionTimeout);
            clients.forEach(c => c.disconnect());
            console.error(error);
            roomsFailed++;
            reject(new Error(error));
        };

        // Create and connect clients for one room
        // Create and connect clients for one room
        for (let i = 0; i < PLAYERS_PER_ROOM; i++) {
            // REMOVE the transport option to allow long-polling fallback
            const client = io(SERVER_URL, { /* transports: ['websocket'] */ }); 

            client.on('connect', () => {
                console.log(`Room ${roomIndex}, Client ${i}: Connected with transport: ${client.io.engine.transport.name}`);
                client.emit('findGame', { numPlayers: PLAYERS_PER_ROOM });
            });
            
            // ... rest of the client setup ...
            
            client.on('connect_error', (err) => {
                // Add more detail to the error log
                console.error(`Full connection error for Room ${roomIndex}, Client ${i}:`, err);
                cleanupAndReject(`Room ${roomIndex}: Connection error for client ${i}: ${err.message}`);
            });
            
            clients.push(client);
        }
    });
}

/**
 * Main function to orchestrate the load test.
 */
async function runLoadTest() {
    console.log(`Starting load test with ${NUM_ROOMS_TO_TEST} concurrent rooms...`);
    const serverStartTime = Date.now();

    const testPromises = [];
    for (let i = 0; i < NUM_ROOMS_TO_TEST; i++) {
        // Stagger the start of each session slightly to avoid a massive connection spike
        await new Promise(res => setTimeout(res, 50)); 
        testPromises.push(createGameSession(i + 1));
    }

    // Wait for all game sessions to complete or fail
    await Promise.allSettled(testPromises);

    const totalDuration = (Date.now() - serverStartTime) / 1000;
    console.log('\n--- Load Test Summary ---');
    console.log(`Total Rooms Simulated: ${NUM_ROOMS_TO_TEST}`);
    console.log(`  - Successful: ${roomsCompleted}`);
    console.log(`  - Failed/Timed Out: ${roomsFailed}`);
    console.log(`Total Test Duration: ${totalDuration.toFixed(2)} seconds`);
    console.log('-------------------------');
}

runLoadTest();