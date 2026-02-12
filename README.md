# Obstrukt

<p align="center">
  <a href="https://obstrukt.vercel.app/">
    <img src="https://github.com/user-attachments/assets/00514b35-75fe-4a92-9fa2-6dc91bca4efe"
         alt="Screenshot of Obstrukt game interface"
         width="100%" />
  </a>
</p>

<p align="center">
  <b><a href="https://obstrukt.vercel.app/">Play Obstrukt</a></b>
</p>

---

## Overview
*Obstrukt* is an online, multiplayer, turn-based board game inspired by *Quoridor*.  
Play against friends in real-time or challenge an AI opponent with adjustable difficulty.

---

## Features

### Implemented
- **Multiplayer mode** – play anonymously over the internet.
- **Two and four player modes**.
- **AI opponents** with configurable difficulty.
- **Board size configuration** – 5×5, 7×7, 9×9, and 11×11.
- **Custom time control** – set time per player.
- **Game history navigation** – step through past moves.

### In Development
- **Player accounts** – persistent profiles and stats.
- **Post-game analysis** – engine-powered evaluation and study tools.
- **Improved AI** – potential neural network + self-play learning.

---

## About the AI
The current AI uses the NegaMax search algorithm, enhanced with:
- Alpha–Beta Pruning
- Null–Move Pruning
- Zobrist Hashing
- Iterative Deepening

The evaluation function is still being tuned, with further optimizations planned to improve response times at higher search depths.

---

## Disclaimer
> *Obstrukt* is an independent, fan-made game inspired by the board game *Quoridor*, created by Gigamic.  
> This project is not affiliated with, endorsed by, or connected to Gigamic in any way.  
> All trademarks and copyrights for *Quoridor* belong to their respective owners.
