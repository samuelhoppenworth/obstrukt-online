// compile with em++ client/src/ai/NegaMax.cpp --bind -o public/ai/ai.js -O3 -s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1

#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <functional>
#include <random>
#include <chrono>
#include <unordered_map>
#include <climits>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

// --- CONFIGURATION ---
const double PATH_SCORE_BASE = 2.0;
const int MAX_EXPECTED_PATH = 16;

// --- DATA STRUCTURES ---
struct PawnPos { 
    int row; 
    int col; 
    bool operator==(const PawnPos& o) const { return row == o.row && col == o.col; } 
    bool operator<(const PawnPos& o) const { return row != o.row ? row < o.row : col < o.col; } 
};

struct Wall { 
    int row; 
    int col; 
    std::string orientation; 
    bool operator==(const Wall& o) const { return row == o.row && col == o.col && orientation == o.orientation; } 
};

struct Move { 
    std::string type; 
    PawnPos pos; 
    Wall wall; 

    bool operator==(const Move& o) const {
        if (type != o.type) {
            return false;
        }
        if (type == "cell") {
            return pos == o.pos;
        }
        if (type == "wall") {
            return wall == o.wall;
        }
        if (type == "resign") {
            return true;
        }
        return false;
    }
};

struct Player { 
    std::string id; 
    std::function<bool(int, int, int)> goalCondition; 
};

struct GameState { 
    int boardSize; 
    std::map<std::string, PawnPos> pawnPositions; 
    std::map<std::string, int> wallsLeft; 
    std::vector<Wall> placedWalls; 
    std::string playerTurn; 
    std::vector<std::string> activePlayerIds; 
    int playerTurnIndex; 
    std::string status = "active"; 
    std::string winner = ""; 
    uint64_t zobristHash = 0; // Zobrist hash for the current state
};

// --- TRANSPOSITION TABLE (TT) IMPLEMENTATION ---
enum TTFlag { EXACT, LOWERBOUND, UPPERBOUND };
struct TTEntry {
    int score;
    int depth;
    TTFlag flag;
};
std::unordered_map<uint64_t, TTEntry> transpositionTable;

// Zobrist Hashing for state-caching
namespace Zobrist {
    const int MAX_BOARD_SIZE = 11;
    const int MAX_PLAYERS = 4;
    std::vector<std::vector<std::vector<uint64_t>>> pawnKeys;
    std::vector<std::vector<uint64_t>> h_wallKeys;
    std::vector<std::vector<uint64_t>> v_wallKeys;
    std::vector<uint64_t> turnKeys;

    void initialize() {
        std::mt19937_64 gen(0xBADF00D); // Fixed seed for determinism
        
        pawnKeys.resize(MAX_PLAYERS, std::vector<std::vector<uint64_t>>(MAX_BOARD_SIZE, std::vector<uint64_t>(MAX_BOARD_SIZE)));
        for (int p = 0; p < MAX_PLAYERS; ++p) {
            for (int r = 0; r < MAX_BOARD_SIZE; ++r) {
                for (int c = 0; c < MAX_BOARD_SIZE; ++c) {
                    pawnKeys[p][r][c] = gen();
                }
            }
        }

        h_wallKeys.resize(MAX_BOARD_SIZE - 1, std::vector<uint64_t>(MAX_BOARD_SIZE - 1));
        v_wallKeys.resize(MAX_BOARD_SIZE - 1, std::vector<uint64_t>(MAX_BOARD_SIZE - 1));
        for (int r = 0; r < MAX_BOARD_SIZE - 1; ++r) {
            for (int c = 0; c < MAX_BOARD_SIZE - 1; ++c) {
                h_wallKeys[r][c] = gen();
                v_wallKeys[r][c] = gen();
            }
        }
        
        turnKeys.resize(MAX_PLAYERS);
        for(int p = 0; p < MAX_PLAYERS; ++p) {
            turnKeys[p] = gen();
        }
    }

    uint64_t computeHash(const GameState& state) {
        uint64_t h = 0;
        for (const auto& pair : state.pawnPositions) {
            int playerIndex = std::distance(state.activePlayerIds.begin(), 
                std::find(state.activePlayerIds.begin(), state.activePlayerIds.end(), pair.first));
            h ^= pawnKeys[playerIndex][pair.second.row][pair.second.col];
        }
        for (const auto& wall : state.placedWalls) {
            if (wall.orientation == "horizontal") {
                h ^= h_wallKeys[wall.row][wall.col];
            } else {
                h ^= v_wallKeys[wall.row][wall.col];
            }
        }
        h ^= turnKeys[state.playerTurnIndex];
        return h;
    }
}

// --- FORWARD DECLARATIONS ---
bool isWallBetween(const std::vector<Wall>& placedWalls, int r1, int c1, int r2, int c2);
bool pathExistsFor(const PawnPos& startPos, const std::function<bool(int, int, int)>& goalCondition, const std::vector<Wall>& placedWalls, int boardSize);
int getShortestPathLength(const PawnPos& startPos, const std::function<bool(int, int, int)>& goalCondition, const std::vector<Wall>& placedWalls, int boardSize);

// --- CORE GAME LOGIC ---

bool isWallBetween(const std::vector<Wall>& placedWalls, int r1, int c1, int r2, int c2) {
    if (c1 == c2) { // Vertical movement
        int wallRow = std::min(r1, r2);
        for (const auto& wall : placedWalls) {
            if (wall.orientation == "horizontal" && wall.row == wallRow && (wall.col == c1 || wall.col == c1 - 1)) return true;
        }
    } else if (r1 == r2) { // Horizontal movement
        int wallCol = std::min(c1, c2);
        for (const auto& wall : placedWalls) {
            if (wall.orientation == "vertical" && wall.col == wallCol && (wall.row == r1 || wall.row == r1 - 1)) return true;
        }
    }
    return false;
}

bool pathExistsFor(const PawnPos& startPos, const std::function<bool(int, int, int)>& goalCondition, const std::vector<Wall>& placedWalls, int boardSize) {
    if (startPos.row == -1) return true;
    return getShortestPathLength(startPos, goalCondition, placedWalls, boardSize) != -1;
}

std::vector<PawnPos> calculateLegalPawnMoves(const std::map<std::string, PawnPos>& pawnPositions, const std::vector<Wall>& placedWalls, const std::vector<Player>& players, const std::vector<std::string>& activePlayerIds, int playerTurnIndex, int boardSize) {
    std::vector<PawnPos> availablePawnMoves;
    if (playerTurnIndex >= activePlayerIds.size()) return availablePawnMoves;
    std::string currentPlayerId = activePlayerIds[playerTurnIndex];
    if (pawnPositions.find(currentPlayerId) == pawnPositions.end()) return availablePawnMoves;

    const PawnPos& currentPos = pawnPositions.at(currentPlayerId);
    int row = currentPos.row;
    int col = currentPos.col;
    
    std::vector<PawnPos> opponentPositions;
    for (const auto& player : players) {
        if (player.id != currentPlayerId && std::find(activePlayerIds.begin(), activePlayerIds.end(), player.id) != activePlayerIds.end()) {
            if (pawnPositions.find(player.id) != pawnPositions.end()) {
                opponentPositions.push_back(pawnPositions.at(player.id));
            }
        }
    }
    
    std::vector<PawnPos> potentialMoves = {{row - 1, col}, {row + 1, col}, {row, col - 1}, {row, col + 1}};
    for (const auto& move : potentialMoves) {
        if (move.row < 0 || move.row >= boardSize || move.col < 0 || move.col >= boardSize) continue;
        
        const PawnPos* opponentInCell = nullptr;
        for (const auto& oppPos : opponentPositions) {
            if (oppPos.row == move.row && oppPos.col == move.col) {
                opponentInCell = &oppPos;
                break;
            }
        }
        
        if (isWallBetween(placedWalls, row, col, move.row, move.col)) continue;
        
        if (opponentInCell) {
            int jumpRow = opponentInCell->row + (opponentInCell->row - row);
            int jumpCol = opponentInCell->col + (opponentInCell->col - col);
            bool wallBehindOpponent = isWallBetween(placedWalls, opponentInCell->row, opponentInCell->col, jumpRow, jumpCol);
            
            if (!wallBehindOpponent && jumpRow >= 0 && jumpRow < boardSize && jumpCol >= 0 && jumpCol < boardSize) {
                availablePawnMoves.push_back({jumpRow, jumpCol});
            } else {
                if (opponentInCell->row == row) { // Horizontal opponent
                    if (!isWallBetween(placedWalls, opponentInCell->row, opponentInCell->col, opponentInCell->row - 1, opponentInCell->col)) availablePawnMoves.push_back({opponentInCell->row - 1, opponentInCell->col});
                    if (!isWallBetween(placedWalls, opponentInCell->row, opponentInCell->col, opponentInCell->row + 1, opponentInCell->col)) availablePawnMoves.push_back({opponentInCell->row + 1, opponentInCell->col});
                } else { // Vertical opponent
                    if (!isWallBetween(placedWalls, opponentInCell->row, opponentInCell->col, opponentInCell->row, opponentInCell->col - 1)) availablePawnMoves.push_back({opponentInCell->row, opponentInCell->col - 1});
                    if (!isWallBetween(placedWalls, opponentInCell->row, opponentInCell->col, opponentInCell->row, opponentInCell->col + 1)) availablePawnMoves.push_back({opponentInCell->row, opponentInCell->col + 1});
                }
            }
        } else {
            availablePawnMoves.push_back({move.row, move.col});
        }
    }
    
    availablePawnMoves.erase(std::remove_if(availablePawnMoves.begin(), availablePawnMoves.end(),
        [&](const PawnPos& m) {
            for (const auto& oppPos : opponentPositions) if (oppPos.row == m.row && oppPos.col == m.col) return true;
            return m.row < 0 || m.row >= boardSize || m.col < 0 || m.col >= boardSize;
        }), availablePawnMoves.end());
    
    return availablePawnMoves;
}

bool isWallPlacementLegal(const Wall& wallData, const GameState& gameState, const std::vector<Player>& players) {
    if (gameState.wallsLeft.at(gameState.playerTurn) <= 0) return false;
    if (wallData.row < 0 || wallData.row > gameState.boardSize - 2 || wallData.col < 0 || wallData.col > gameState.boardSize - 2) return false;
    
    for (const auto& wall : gameState.placedWalls) {
        if (wall.row == wallData.row && wall.col == wallData.col) return false;
        if (wallData.orientation == "horizontal" && wall.orientation == "horizontal" && wall.row == wallData.row && std::abs(wall.col - wallData.col) < 2) return false;
        if (wallData.orientation == "vertical" && wall.orientation == "vertical" && wall.col == wallData.col && std::abs(wall.row - wallData.row) < 2) return false;
    }
    
    std::vector<Wall> tempPlacedWalls = gameState.placedWalls;
    tempPlacedWalls.push_back(wallData);
    
    for (const std::string& playerId : gameState.activePlayerIds) {
        const Player* player = nullptr;
        for (const auto& p : players) if (p.id == playerId) player = &p;
        if (player && gameState.pawnPositions.count(player->id)) {
            if (!pathExistsFor(gameState.pawnPositions.at(player->id), player->goalCondition, tempPlacedWalls, gameState.boardSize)) return false;
        }
    }
    
    return true;
}

GameState switchTurn(GameState gameState) {
    // Update hash for turn switch
    gameState.zobristHash ^= Zobrist::turnKeys[gameState.playerTurnIndex];
    gameState.playerTurnIndex = (gameState.playerTurnIndex + 1) % gameState.activePlayerIds.size();
    gameState.playerTurn = gameState.activePlayerIds[gameState.playerTurnIndex];
    gameState.zobristHash ^= Zobrist::turnKeys[gameState.playerTurnIndex];
    return gameState;
}

GameState applyPawnMove(GameState gameState, const PawnPos& moveData, const std::vector<Player>& players) {
    std::string currentPlayerId = gameState.playerTurn;
    int playerIndex = gameState.playerTurnIndex;
    
    // Update hash for pawn move
    PawnPos oldPos = gameState.pawnPositions[currentPlayerId];
    gameState.zobristHash ^= Zobrist::pawnKeys[playerIndex][oldPos.row][oldPos.col];
    gameState.zobristHash ^= Zobrist::pawnKeys[playerIndex][moveData.row][moveData.col];

    gameState.pawnPositions[currentPlayerId] = moveData;
    
    const Player* currentPlayer = &players[playerIndex];
    if (currentPlayer->goalCondition(moveData.row, moveData.col, gameState.boardSize)) {
        gameState.status = "ended";
        gameState.winner = currentPlayerId;
    } else {
        gameState = switchTurn(gameState);
    }
    return gameState;
}

GameState applyWallPlacement(GameState gameState, const Wall& wallData) {
    // Update hash for wall placement
    if (wallData.orientation == "horizontal") {
        gameState.zobristHash ^= Zobrist::h_wallKeys[wallData.row][wallData.col];
    } else {
        gameState.zobristHash ^= Zobrist::v_wallKeys[wallData.row][wallData.col];
    }

    gameState.placedWalls.push_back(wallData);
    gameState.wallsLeft[gameState.playerTurn]--;
    gameState = switchTurn(gameState);
    return gameState;
}

GameState applyMove(GameState gameState, const Move& move, const std::vector<Player>& players) {
    if (move.type == "cell") {
        return applyPawnMove(gameState, move.pos, players);
    } else if (move.type == "wall") {
        return applyWallPlacement(gameState, move.wall);
    }
    return gameState;
}

// --- AI LOGIC ---//

int getShortestPathLength(const PawnPos& startPos, const std::function<bool(int, int, int)>& goalCondition, const std::vector<Wall>& placedWalls, int boardSize) {
    if (goalCondition(startPos.row, startPos.col, boardSize)) return 0;
    
    std::queue<std::pair<PawnPos, int>> queue;
    bool visited[Zobrist::MAX_BOARD_SIZE][Zobrist::MAX_BOARD_SIZE] = {false};

    queue.push({startPos, 0});
    visited[startPos.row][startPos.col] = true;
    
    while (!queue.empty()) {
        PawnPos currentPos = queue.front().first;
        int distance = queue.front().second;
        queue.pop();
                
        std::vector<PawnPos> neighbors = {
            {currentPos.row - 1, currentPos.col}, 
            {currentPos.row + 1, currentPos.col}, 
            {currentPos.row, currentPos.col - 1}, 
            {currentPos.row, currentPos.col + 1}
        };

        for (const auto& neighbor : neighbors) {
            if (neighbor.row >= 0 && neighbor.row < boardSize && neighbor.col >= 0 && neighbor.col < boardSize && 
                !isWallBetween(placedWalls, currentPos.row, currentPos.col, neighbor.row, neighbor.col) && 
                !visited[neighbor.row][neighbor.col]) {
                
                // If we found a goal, we can return immediately. This is the shortest path.
                if (goalCondition(neighbor.row, neighbor.col, boardSize)) return distance + 1;

                visited[neighbor.row][neighbor.col] = true;
                queue.push({neighbor, distance + 1});
            }
        }
    }
    return -1;
}

// ** FINAL STRATEGIC EVALUATION FUNCTION **
int evaluate(const GameState& state, const std::vector<Player>& players) {
    if (state.status == "ended") {
        return (state.winner == state.playerTurn) ? INT_MAX : -INT_MAX;
    }

    std::string myId = state.playerTurn;
    const Player* myPlayer = nullptr;
    for(const auto& p : players) {
        if (p.id == myId) myPlayer = &p;
    }

    int myPath = getShortestPathLength(state.pawnPositions.at(myId), myPlayer->goalCondition, state.placedWalls, state.boardSize);

    // --- Heuristic: Shortest Path Difference (vs. most threatening opponent) ---
    int mostThreateningOpponentPath = MAX_EXPECTED_PATH + 1;
    for (const std::string& opponentId : state.activePlayerIds) {
        if (opponentId == myId) continue;

        const Player* opponentPlayer = nullptr;
        for(const auto& p : players) { if (p.id == opponentId) opponentPlayer = &p; }

        if (opponentPlayer && state.pawnPositions.count(opponentId)) {
            int opponentPath = getShortestPathLength(state.pawnPositions.at(opponentId), opponentPlayer->goalCondition, state.placedWalls, state.boardSize);
            if (opponentPath != -1) {
                mostThreateningOpponentPath = std::min(mostThreateningOpponentPath, opponentPath);
            }
        }
    }
    
    // Exponential scaling makes path differences more critical near the goal line.
    double myProgressScore = pow(PATH_SCORE_BASE, MAX_EXPECTED_PATH - myPath);
    double opponentProgressScore = pow(PATH_SCORE_BASE, MAX_EXPECTED_PATH - mostThreateningOpponentPath);
    int pathScore = static_cast<int>(myProgressScore - opponentProgressScore);

    // --- Heuristic: Wall Conservation & Wall Difference ---
    // This logic makes walls more valuable in the early/mid game, discouraging the AI from wasting them.
    // It also implicitly rewards having more walls than opponents (wall difference).
    int totalWallsOnBoard = (10 * state.activePlayerIds.size());
    for(const auto& pair : state.wallsLeft) { totalWallsOnBoard -= pair.second; }
    
    // The multiplier is high when few walls are placed, and low when many are.
    int wallScoreMultiplier = 5 + (40 - totalWallsOnBoard) / 4; // Assumes 40 max walls for 4P
    int wallAdvantageScore = state.wallsLeft.at(myId) * wallScoreMultiplier;

    return pathScore + wallAdvantageScore;
}


std::vector<Move> generateAndOrderMoves(const GameState& state, const std::vector<Player>& players) {
    std::vector<std::pair<Move, int>> scoredMoves;
    std::string myId = state.playerTurn;

    const Player* myPlayer = nullptr;
    for(const auto& p : players) { if(p.id == myId) myPlayer = &p; }
    if (!myPlayer) return {}; // Cannot move if not a player

    // --- Identify the single most threatening opponent ---
    const Player* opponentPlayer = nullptr;
    std::string opponentId = "";
    int initialOpponentPath = MAX_EXPECTED_PATH + 1;

    for (const auto& id : state.activePlayerIds) {
        if (id == myId) continue;
        const Player* tempOpponent = nullptr;
        for(const auto& p : players) { if(p.id == id) tempOpponent = &p; }

        if(tempOpponent && state.pawnPositions.count(id)) {
            int path = getShortestPathLength(state.pawnPositions.at(id), tempOpponent->goalCondition, state.placedWalls, state.boardSize);
            if (path != -1 && path < initialOpponentPath) {
                initialOpponentPath = path;
                opponentId = id;
                opponentPlayer = tempOpponent;
            }
        }
    }

    int initialMyPath = getShortestPathLength(state.pawnPositions.at(myId), myPlayer->goalCondition, state.placedWalls, state.boardSize);

    // --- 1. Score and Generate Pawn Moves (Heuristic: Forward Progress) ---
    // Pawn moves are the default good move
    std::vector<PawnPos> pawnMoves = calculateLegalPawnMoves(state.pawnPositions, state.placedWalls, players, state.activePlayerIds, state.playerTurnIndex, state.boardSize);
    for (const auto& pos : pawnMoves) {
        int newMyPath = getShortestPathLength(pos, myPlayer->goalCondition, state.placedWalls, state.boardSize);

        if (myPlayer->goalCondition(pos.row, pos.col, state.boardSize)) {
            scoredMoves.push_back({{"cell", pos, {}}, INT_MAX});
            continue;
        }

        // Score based on how much closer to the goal the pawn gets.
        int score = (initialMyPath - newMyPath); 
        scoredMoves.push_back({{"cell", pos, {}}, 10000 + score * 100});
    }

    // --- 2. Score and Generate Wall Moves (Heuristics: Blocking & Self-Preservation) ---
    if (state.wallsLeft.count(myId) && state.wallsLeft.at(myId) > 0 && opponentPlayer) {
        for (int r = 0; r <= state.boardSize - 2; ++r) {
            for (int c = 0; c <= state.boardSize - 2; ++c) {
                Wall walls[] = {{r, c, "horizontal"}, {r, c, "vertical"}};
                for (const auto& wall : walls) {
                    if (isWallPlacementLegal(wall, state, players)) {
                        std::vector<Wall> tempWalls = state.placedWalls;
                        tempWalls.push_back(wall);

                        // Check if the wall hurts self
                        int newMyPathAfterWall = getShortestPathLength(state.pawnPositions.at(myId), myPlayer->goalCondition, tempWalls, state.boardSize);
                        if (newMyPathAfterWall == -1 || newMyPathAfterWall > initialMyPath) {
                            continue; // Ignore self-blocking walls.
                        }
                        
                        // Calculate how much this wall hinders the most threatening opponent.
                        int newOpponentPath = getShortestPathLength(state.pawnPositions.at(opponentId), opponentPlayer->goalCondition, tempWalls, state.boardSize);
                        
                        if (newOpponentPath != -1) {
                            int opponentPathIncrease = newOpponentPath - initialOpponentPath;

                            if (opponentPathIncrease > 0) {
                                // --- Heuristic: Edge Case - Emergency Block ---
                                if (initialOpponentPath <= 2) {
                                     scoredMoves.push_back({{"wall", {}, wall}, 50000 + opponentPathIncrease * 1000});
                                } else {
                                     scoredMoves.push_back({{"wall", {}, wall}, opponentPathIncrease * 200});
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Sort moves: Best moves (highest score) first
    std::sort(scoredMoves.begin(), scoredMoves.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    // Extract sorted moves
    std::vector<Move> sortedMoves;
    for(const auto& scoredMove : scoredMoves) {
        sortedMoves.push_back(scoredMove.first);
    }
    
    return sortedMoves;
}

// Basic, vanilla minimax algorithm used for benchmarking purposes
int minimax(GameState state, int depth, bool maximizingPlayer, const std::vector<Player>& players, int ply) {
    if (depth == 0 || state.status == "ended") {
        int base_score = evaluate(state, players);
        // Adjust score for wins/losses based on how many moves it took
        if (base_score == INT_MAX) base_score -= ply;
        if (base_score == -INT_MAX) base_score += ply;
        return base_score;
    }

    std::vector<Move> moves = generateAndOrderMoves(state, players);
    if (moves.empty()) {
        return evaluate(state, players);
    }

    if (maximizingPlayer) {
        int maxEval = -INT_MAX;
        for (const auto& move : moves) {
            GameState nextState = applyMove(state, move, players);
            int eval = minimax(nextState, depth - 1, false, players, ply + 1);
            maxEval = std::max(maxEval, eval);
        }
        return maxEval;
    } else { // Minimizing player
        int minEval = INT_MAX;
        for (const auto& move : moves) {
            GameState nextState = applyMove(state, move, players);
            // In a 2-player game, the next state is for the other player.
            // For simplicity in this vanilla implementation, we assume the next turn is always a maximizing player.
            // A more robust implementation would handle multiple opponents differently.
            int eval = minimax(nextState, depth - 1, true, players, ply + 1);
            minEval = std::min(minEval, eval);
        }
        return minEval;
    }
}

int negamax(GameState state, int depth, int alpha, int beta, int color, const std::vector<Player>& players, int ply, 
            bool useAlphaBeta, bool useNullMovePruning, bool useTranspositionTable) {
    
    int alphaOrig = alpha;

    // --- 1. Transposition Table Lookup ---
    if (useTranspositionTable) {
        auto it = transpositionTable.find(state.zobristHash);
        if (it != transpositionTable.end() && it->second.depth >= depth) {
            TTEntry entry = it->second;
            int score = entry.score;
            if (score > 900000) score -= ply;
            if (score < -900000) score += ply;

            if (entry.flag == EXACT) return score;
            if (entry.flag == LOWERBOUND) alpha = std::max(alpha, score);
            else if (entry.flag == UPPERBOUND) beta = std::min(beta, score);
            
            if (useAlphaBeta && alpha >= beta) return score;
        }
    }

    if (state.status == "ended" || depth == 0) {
        int base_score = evaluate(state, players);
        if (base_score > 900000) base_score -= ply;
        if (base_score < -900000) base_score += ply;
        return color * base_score;
    }

    // --- 2. Null Move Pruning ---
    const int R = 3; 
    if (useNullMovePruning && depth >= R + 1 && state.wallsLeft.at(state.playerTurn) > 0) {
        GameState tempState = switchTurn(state);
        int null_move_score = -negamax(tempState, depth - 1 - R, -beta, -beta + 1, -color, players, ply + 1, useAlphaBeta, useNullMovePruning, useTranspositionTable);

        if (useAlphaBeta && null_move_score >= beta) {
            return beta; 
        }
    }

    // --- Search Logic ---
    std::vector<Move> moves = generateAndOrderMoves(state, players);
    if (moves.empty()) {
        return color * evaluate(state, players);
    }

    int maxVal = -INT_MAX;
    for (const auto& move : moves) {
        GameState nextState = applyMove(state, move, players);
        
        // Pass alpha-beta bounds based on whether the optimization is active
        int next_alpha = useAlphaBeta ? -beta : -INT_MAX;
        int next_beta = useAlphaBeta ? -alpha : INT_MAX;

        // Recursive call with flags
        int val = -negamax(nextState, depth - 1, next_alpha, next_beta, -color, players, ply + 1, useAlphaBeta, useNullMovePruning, useTranspositionTable); 
        
        maxVal = std::max(maxVal, val);
        alpha = std::max(alpha, val);
        
        // --- Alpha-Beta Pruning Check ---
        if (useAlphaBeta && alpha >= beta) {
            break; // Pruning
        }
    }

    // --- Transposition Table Store ---
    if (useTranspositionTable) {
        TTEntry new_entry;
        new_entry.score = maxVal; 
        new_entry.depth = depth;
        if (maxVal <= alphaOrig) new_entry.flag = UPPERBOUND;
        else if (maxVal >= beta) new_entry.flag = LOWERBOUND;
        else new_entry.flag = EXACT;
        transpositionTable[state.zobristHash] = new_entry;
    }
    
    return maxVal;
}

GameState jsToCppState(const emscripten::val& jsState) {
    GameState state;
    state.boardSize = jsState["boardSize"].as<int>();
    state.playerTurn = jsState["playerTurn"].as<std::string>();
    state.playerTurnIndex = jsState["playerTurnIndex"].as<int>();
    state.status = jsState["status"].as<std::string>();
    state.activePlayerIds = emscripten::vecFromJSArray<std::string>(jsState["activePlayerIds"]);

    emscripten::val jsPawnPositions = jsState["pawnPositions"];
    std::vector<std::string> pawnKeys = emscripten::vecFromJSArray<std::string>(emscripten::val::global("Object").call<emscripten::val>("keys", jsPawnPositions));
    for (const auto& key : pawnKeys) {
        state.pawnPositions[key] = {jsPawnPositions[key]["row"].as<int>(), jsPawnPositions[key]["col"].as<int>()};
    }
    
    emscripten::val jsWallsLeft = jsState["wallsLeft"];
    std::vector<std::string> wallKeys = emscripten::vecFromJSArray<std::string>(emscripten::val::global("Object").call<emscripten::val>("keys", jsWallsLeft));
    for (const auto& key : wallKeys) {
        state.wallsLeft[key] = jsWallsLeft[key].as<int>();
    }
    
    if (jsState.hasOwnProperty("placedWalls") && !jsState["placedWalls"].isUndefined()) {
        emscripten::val jsPlacedWalls = jsState["placedWalls"];
        for (int i = 0; i < jsPlacedWalls["length"].as<int>(); ++i) {
            state.placedWalls.push_back({jsPlacedWalls[i]["row"].as<int>(), jsPlacedWalls[i]["col"].as<int>(), jsPlacedWalls[i]["orientation"].as<std::string>()});
        }
    }
    
    // Important: Compute the initial hash for the state received from JS
    state.zobristHash = Zobrist::computeHash(state);

    return state;
}

std::vector<Player> jsToCppPlayers(const emscripten::val& jsPlayers) {
    std::vector<Player> players;
    if (jsPlayers.isUndefined()) return players;
    
    for (int i = 0; i < jsPlayers["length"].as<int>(); ++i) {
        std::string id = jsPlayers[i]["id"].as<std::string>();
        Player player;
        player.id = id;
        
        if (id == "p1") player.goalCondition = [](int r, int c, int boardSize) { return r == 0; };
        else if (id == "p2") player.goalCondition = [](int r, int c, int boardSize) { return c == 0; };
        else if (id == "p3") player.goalCondition = [](int r, int c, int boardSize) { return r == boardSize - 1; };
        else if (id == "p4") player.goalCondition = [](int r, int c, int boardSize) { return c == boardSize - 1; };
        
        players.push_back(player);
    }
    return players;
}

emscripten::val cppMoveToJs(const Move& move) {
    emscripten::val jsMove = emscripten::val::object();
    jsMove.set("type", move.type);
    
    if (move.type != "resign") {
        emscripten::val data = emscripten::val::object();
        if (move.type == "cell") {
            data.set("row", move.pos.row);
            data.set("col", move.pos.col);
        } else if (move.type == "wall") {
            data.set("row", move.wall.row);
            data.set("col", move.wall.col);
            data.set("orientation", move.wall.orientation);
        }
        jsMove.set("data", data);
    }
    return jsMove;
}

emscripten::val findBestMove(const emscripten::val& jsState, const emscripten::val& jsPlayers, int targetDepth) {
    GameState state = jsToCppState(jsState);
    std::vector<Player> players = jsToCppPlayers(jsPlayers);
    
    // Use the passed-in depth, with a fallback to a reasonable default.
    const int TARGET_DEPTH = (targetDepth > 0) ? targetDepth : 4; 

    // Generate the initial list of moves just once.
    std::vector<Move> movesToSearch = generateAndOrderMoves(state, players);
    if (movesToSearch.empty()) {
        return cppMoveToJs({"resign", {}, {}});
    }

    Move bestMoveOverall = movesToSearch[0];

    // --- ITERATIVE DEEPENING LOOP ---
    for (int current_depth = 1; current_depth <= TARGET_DEPTH; ++current_depth) {
        Move bestMoveThisIteration = movesToSearch[0];
        int bestValue = -INT_MAX;

        // 'movesToSearch' vector is ordered from the previous iteration's results.
        for (const auto& move : movesToSearch) {
            GameState nextState = applyMove(state, move, players);
            int value = -negamax(   nextState, 
                                    current_depth - 1, 
                                    -INT_MAX, 
                                    INT_MAX, 
                                    -1, 
                                    players, 
                                    1, 
                                    true, 
                                    true, 
                                    true
                                );
            if (value > bestValue) {
                bestValue = value;
                bestMoveThisIteration = move;
            }
        }
        bestMoveOverall = bestMoveThisIteration;

        // Re-order the moves list for the next, deeper search.
        // Find the best move from the completed iterationand move it to the front.
        auto it = std::find(movesToSearch.begin(), movesToSearch.end(), bestMoveThisIteration);
        if (it != movesToSearch.begin()) {
            std::rotate(movesToSearch.begin(), it, it + 1);
        }
    }
    
    return cppMoveToJs(bestMoveOverall);
}

emscripten::val runAblationBenchmark(const emscripten::val& jsState, const emscripten::val& jsPlayers, int depth) {
    GameState state = jsToCppState(jsState);
    std::vector<Player> players = jsToCppPlayers(jsPlayers);

    emscripten::val results_array = emscripten::val::array();

    // Loop through all 8 combinations of the three optimizations
    for (int i = 0; i < 8; ++i) {
        bool useAlphaBeta = (i & 1) != 0;
        bool useNullMovePruning = (i & 2) != 0;
        bool useTranspositionTable = (i & 4) != 0;

        // Invalidate NMP if AB pruning is disabled, as NMP relies on a tight beta bound
        if (!useAlphaBeta && useNullMovePruning) continue;

        std::string configName = "NegaMax";
        if (useAlphaBeta) configName += " +AB";
        if (useNullMovePruning) configName += " +NMP";
        if (useTranspositionTable) configName += " +TT";
        if (i == 0) configName = "Vanilla NegaMax (None)";

        // Clear TT before each run for a fair test
        transpositionTable.clear();

        auto startTime = std::chrono::high_resolution_clock::now();
        int score = negamax(state, depth, -INT_MAX, INT_MAX, 1, players, 0, useAlphaBeta, useNullMovePruning, useTranspositionTable);
        auto endTime = std::chrono::high_resolution_clock::now();
        long long duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        emscripten::val result_obj = emscripten::val::object();
        result_obj.set("name", configName);
        result_obj.set("timeMs", duration);
        result_obj.set("score", score);
        results_array.call<void>("push", result_obj);
    }

    return results_array;
}

EMSCRIPTEN_BINDINGS(quoridor_ai_module) {
    // Call Zobrist initialization once when the module loads
    Zobrist::initialize();
    emscripten::function("findBestMove", &findBestMove, emscripten::allow_raw_pointers());
    emscripten::function("runAblationBenchmark", &runAblationBenchmark, emscripten::allow_raw_pointers());
}