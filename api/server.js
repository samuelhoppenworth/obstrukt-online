// server/server.js
import express from 'express';
import http from 'http';
import { Server } from 'socket.io';
import GameManager from '../server/GameManager.js';
// Corrected import path to use the more robust, shared gameConfig
import { ALL_PLAYERS } from '../client/src/config/gameConfig.js'; 

const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: "*", methods: ["GET", "POST"] } });

const matchmakingQueues = {
    '2': [],
    '4': [],
};
const rooms = {};

const createNewGame = (sockets, numPlayers) => {
    const roomName = `room-${sockets.map(s => s.id.slice(0, 4)).join('-')}`;
    console.log(`Match found. Creating ${numPlayers}-player game in room: ${roomName}`);

    const playersForGame = numPlayers === 2
        ? [ALL_PLAYERS[0], ALL_PLAYERS[2]] // P1 vs P3 for 2-player
        : ALL_PLAYERS.slice(0, 4);        // P1, P2, P3, P4 for 4-player

    const playerMap = {};
    sockets.forEach((socket, i) => {
        socket.join(roomName);
        playerMap[socket.id] = playersForGame[i].id;
    });

    const config = {
        numPlayers: numPlayers,
        players: playersForGame,
        wallsPerPlayer: numPlayers === 2 ? 10 : 5,
        timePerPlayer: 5 * 60 * 1000,
        boardSize: 9,
    };

    // The server emitter uses socket.io to broadcast events
    const gameEmitter = { emit: (event, data) => io.to(roomName).emit(event, data) };
    const gameManager = new GameManager(gameEmitter, config);
    
    rooms[roomName] = {
        players: playerMap,
        gameManager: gameManager,
    };

    io.to(roomName).emit('gameStart', {
        room: roomName,
        playerMap: playerMap,
        initialState: gameManager.getGameState(),
        config: config
    });

    gameManager.startServerTimer();
};

io.on('connection', (socket) => {
    console.log('User connected:', socket.id);

    socket.on('findGame', ({ numPlayers }) => {
        const queueName = numPlayers.toString();
        if (!matchmakingQueues[queueName]) {
            return socket.emit('error', 'Invalid game mode requested.');
        }

        console.log(`Player ${socket.id} is looking for a ${queueName}-player game.`);
        socket.emit('waiting', `Waiting for ${numPlayers - 1} more player(s)...`);
        
        const queue = matchmakingQueues[queueName];
        queue.push(socket);

        if (queue.length >= numPlayers) {
            const playersForGame = queue.splice(0, numPlayers);
            createNewGame(playersForGame, numPlayers);
        }
    });

    socket.on('requestMove', (moveData) => {
        const roomName = Array.from(socket.rooms)[1];
        if (!roomName || !rooms[roomName]) return;

        const { gameManager, players } = rooms[roomName];
        const playerRole = players[socket.id];
        
        if (playerRole === gameManager.gameState.playerTurn) {
            const moveMade = gameManager.handleMoveRequest(moveData);
            if (moveMade) {
                // Since a valid move was made, any pending draw offers are automatically rescinded
                io.to(roomName).emit('drawOfferRescinded');
            }
        } else {
            socket.emit('error', 'Not your turn.');
        }
    });

    socket.on('resign', () => {
        const roomName = Array.from(socket.rooms)[1];
        if (!roomName || !rooms[roomName]) return;
        const { gameManager, players } = rooms[roomName];
        const playerRole = players[socket.id];
        if (playerRole) {
            gameManager.handlePlayerLoss(playerRole, 'resignation');
        }
    });

    socket.on('requestDraw', () => {
        const roomName = Array.from(socket.rooms)[1];
        if (!roomName || !rooms[roomName]) return;
        
        const { gameManager, players } = rooms[roomName];
        const playerRole = players[socket.id];

        if (gameManager.config.numPlayers !== 2) return socket.emit('error', 'Draw offers only supported in 2-player games.');
        
        const opponentSocketId = Object.keys(players).find(id => id !== socket.id);
        if (!opponentSocketId) return;

        gameManager.gameState.drawOfferFrom = playerRole;

        io.to(opponentSocketId).emit('drawOfferReceived', { from: playerRole });
        socket.emit('drawOfferPending');
    });

    socket.on('respondToDraw', ({ accepted }) => {
        const roomName = Array.from(socket.rooms)[1];
        if (!roomName || !rooms[roomName]) return;

        const { gameManager, players } = rooms[roomName];
        const offererId = gameManager.gameState.drawOfferFrom;

        if (!offererId) return socket.emit('error', 'There is no active draw offer.');

        if (accepted) {
            gameManager.endGameAsDraw('draw by agreement');
        } else {
            gameManager.gameState.drawOfferFrom = null;
            io.to(roomName).emit('drawOfferRescinded');
        }
    });

    socket.on('disconnect', () => {
        console.log('User disconnected:', socket.id);

        for (const queueName in matchmakingQueues) {
            matchmakingQueues[queueName] = matchmakingQueues[queueName].filter(s => s.id !== socket.id);
        }
        
        const roomName = Object.keys(rooms).find(r => rooms[r] && Object.keys(rooms[r].players).includes(socket.id));
        if (roomName && rooms[roomName]) {
            const { gameManager, players } = rooms[roomName];
            const playerRole = players[socket.id];

            if (gameManager.gameState.status === 'active') {
                console.log(`Player ${playerRole} disconnected from active game in ${roomName}.`);
                gameManager.handlePlayerLoss(playerRole, 'disconnection');
            }

            // Clean up empty room
            const remainingPlayers = Object.keys(players).filter(pid => pid !== socket.id);
            if (remainingPlayers.length === 0) {
                console.log(`Room ${roomName} is empty, deleting.`);
                if (rooms[roomName].gameManager.timerInterval) {
                    clearInterval(rooms[roomName].gameManager.timerInterval);
                }
                delete rooms[roomName];
            }
        }
    });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log(`Server listening on port ${PORT}`));
