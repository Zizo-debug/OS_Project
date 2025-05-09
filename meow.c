#include <SFML/Graphics.h>
#include <SFML/System.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h> 
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <semaphore.h>

#define CELL_SIZE 50
#define ROWS 20
#define COLS 20
#define HUD_HEIGHT 60
#define WINDOW_WIDTH (COLS * CELL_SIZE)
#define WINDOW_HEIGHT (ROWS * CELL_SIZE + HUD_HEIGHT)

pthread_cond_t gameEngineThreadExitCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t gameEngineThreadExitMutex = PTHREAD_MUTEX_INITIALIZER;
bool gameEngineThreadExited = false;
pthread_mutex_t ghostExitMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ghostExitCond = PTHREAD_COND_INITIALIZER;
int ghostThreadsExited = 0;

// New: Semaphores for keys and exit permits
#define MAX_KEYS 2
#define MAX_EXIT_PERMITS 2
sem_t keySemaphore;
sem_t exitPermitSemaphore;

// Direction constants
typedef enum {
    DIR_NONE = 0,
    DIR_UP,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} Direction;

// Game screen states
typedef enum {
    SCREEN_MENU,
    SCREEN_PLAY,
    SCREEN_SCOREBOARD,
    SCREEN_INSTRUCTIONS,
    SCREEN_QUIT,
    SCREEN_GAME_OVER
} GameScreen;

// UI state structure
typedef struct {
    GameScreen currentScreen;
    int selectedMenuItem;
    bool needsRedraw;
    pthread_mutex_t mutex;
    char username[32];
    bool enteringUsername;
    int usernameCursorPos;
} UIState;

// Ghost structure to track each ghost's state
typedef struct {
    int row;
    int col;
    int id;
    Direction direction;
    bool isVulnerable;
    pthread_t thread;
    bool isActive;
    bool needsRespawn;
    int respawnRow;
    int respawnCol;
    int ghostType;
    bool hasKey;          // New: Tracks if ghost has a key
    bool hasExitPermit;   // New: Tracks if ghost has an exit permit
    bool inGhostHouse;    // New: Tracks if ghost is in the ghost house
} Ghost;

// Ghost variables
#define MAX_GHOSTS 4
Ghost ghosts[MAX_GHOSTS];
pthread_mutex_t ghostMutex = PTHREAD_MUTEX_INITIALIZER;
bool ghostThreadsRunning = true;

// Game state structure
typedef struct {
    char board[20][20];
    char originalBoard[20][20];
    int score;
    int lives;
    int pacmanRow;
    int pacmanCol;
    float pacmanRotation;
    Direction currentDirection;
    bool gameRunning;
    bool gamePaused;
    pthread_mutex_t mutex;
    bool powerPelletActive;
    float powerPelletDuration;
    bool ghostVulnerable;
    float ghostVulnerableDuration;
    int pacmanStartRow;
    int pacmanStartCol;
} GameState;

GameState gameState;
UIState uiState;

// Menu items
const char* menuItems[] = {
    "Play",
    "Scoreboard",
    "Instructions",
    "Quit"
};
#define MENU_ITEM_COUNT 4

char initialBoard[20][20] = {
    "====================",
    "=0...............0.=",
    "=.====.======.====.=",
    "=.=............=.=.=",
    "=.=.==..####...=...=",
    "=...==.=======.==..=",
    "====== =     =.=====",
    "=..... =     =.....=",
    "=.==== === ===.=====",
    "=......=.....=.....=",
    "=.==== ==....====. =",
    "=.==== ==..... === =",
    "=..... ==.===. ....=",
    "=.==== ==.===. =====",
    "=.==== ....... =====",
    "=......======......=",
    "=.====.======.====.=",
    "=0.==............0.=",
    "=...........@......=",
    "===================="
};

sfClock* gameClock;
sfClock* pelletBlinkClock;

// Texture variables
sfTexture* ghost1Texture;
sfTexture* ghost5Texture;
sfTexture* ghost2Texture;
sfTexture* ghost3Texture;
sfTexture* ghost4Texture;

float pelletBlinkInterval = 0.3f;
int pelletVisible = 1;

// Input event queue
#define MAX_INPUT_EVENTS 10
typedef struct {
    int eventType;
    int data;
    bool processed;
} InputEvent;

InputEvent inputEventQueue[MAX_INPUT_EVENTS];
int eventQueueHead = 0;
int eventQueueTail = 0;
pthread_mutex_t eventQueueMutex = PTHREAD_MUTEX_INITIALIZER;

// Event types
#define EVENT_DIRECTION_CHANGE 0
#define EVENT_MENU_SELECT 1
#define EVENT_SCREEN_CHANGE 2

// Function declarations
void processInput(sfRenderWindow* window);
void renderInstructions(sfRenderWindow* window, sfFont* font);
void renderMenu(sfRenderWindow* window, sfFont* font);
void renderScoreboard(sfRenderWindow* window, sfFont* font);
void renderGame(sfRenderWindow* window, sfRectangleShape* wall, sfCircleShape* dot, 
               sfCircleShape* powerPellet, sfSprite* pacmanSprite, sfSprite* ghost1Sprite,
               sfSprite* ghost2Sprite, sfSprite* ghost3Sprite, sfSprite* ghost4Sprite,
               sfSprite* ghost5Sprite, sfText* scoreText, sfText* livesText,
               sfSprite* lifeSprite);

#define SCORE_FILE "scores.txt"
#define MAX_SCORES 10

// Probability weights for ghost direction choices
typedef struct {
    float up;
    float down;
    float left;
    float right;
} DirectionWeights;

typedef struct {
    char username[32];
    int score;
} ScoreEntry;
ScoreEntry scoreBoard[MAX_SCORES];
int scoreCount = 0;

void loadScores() {
    FILE* file = fopen(SCORE_FILE, "r");
    if (file == NULL) {
        scoreCount = 0;
        return;
    }

    scoreCount = 0;
    while (fscanf(file, "%31s %d", 
                 scoreBoard[scoreCount].username, 
                 &scoreBoard[scoreCount].score) == 2) {
        scoreCount++;
        if (scoreCount >= MAX_SCORES) {
            break;
        }
    }
    fclose(file);
}

void saveScores() {
    FILE* file = fopen(SCORE_FILE, "w");
    if (file == NULL) {
        printf("Error opening score file for writing.\n");
        return;
    }

    for (int i = 0; i < scoreCount; i++) {
        fprintf(file, "%s %d\n", scoreBoard[i].username, scoreBoard[i].score);
    }
    fclose(file);
}

void addScore(const char* username, int score) {
    if (scoreCount < MAX_SCORES || score > scoreBoard[scoreCount-1].score) {
        int insertPos = scoreCount;
        for (int i = 0; i < scoreCount; i++) {
            if (score > scoreBoard[i].score) {
                insertPos = i;
                break;
            }
        }

        if (scoreCount == MAX_SCORES) {
            insertPos = MAX_SCORES - 1;
        }

        if (scoreCount < MAX_SCORES) {
            scoreCount++;
        }

        for (int i = scoreCount-1; i > insertPos; i--) {
            scoreBoard[i] = scoreBoard[i-1];
        }

        strncpy(scoreBoard[insertPos].username, username, 31);
        scoreBoard[insertPos].username[31] = '\0';
        scoreBoard[insertPos].score = score;

        saveScores();
    }
}

void saveOriginalBoard() {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            if (gameState.board[i][j] != '#' && gameState.board[i][j] != '@') {
                gameState.originalBoard[i][j] = gameState.board[i][j];
            } else {
                gameState.originalBoard[i][j] = ' ';
            }
        }
    }
}

void initGhosts() {
    int ghostStartPositions[MAX_GHOSTS][2] = {
        {4, 7},
        {4, 8},
        {4, 9},
        {4, 10}
    };
    
    pthread_mutex_lock(&gameState.mutex);
    
    for (int i = 0; i < MAX_GHOSTS; i++) {
        ghosts[i].row = ghostStartPositions[i][0];
        ghosts[i].col = ghostStartPositions[i][1];
        ghosts[i].id = i;
        ghosts[i].direction = DIR_NONE;
        ghosts[i].isVulnerable = false;
        ghosts[i].isActive = true;
        ghosts[i].needsRespawn = false;
        ghosts[i].respawnRow = ghostStartPositions[i][0];
        ghosts[i].respawnCol = ghostStartPositions[i][1];
        ghosts[i].ghostType = i + 1;
        ghosts[i].hasKey = false;           // New: Initialize resource flags
        ghosts[i].hasExitPermit = false;
        ghosts[i].inGhostHouse = true;      // New: Ghosts start in ghost house
        
        gameState.board[ghosts[i].row][ghosts[i].col] = '#';
    }
    
    pthread_mutex_unlock(&gameState.mutex);
}

bool isValidGhostMove(int row, int col) {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) {
        return false;
    }
    
    char cell = gameState.board[row][col];
    return (cell != '=' && cell != '#');
}

DirectionWeights calculateDirectionWeights(Ghost* ghost, int targetRow, int targetCol) {
    DirectionWeights weights = {1.0f, 1.0f, 1.0f, 1.0f};
    
    int currentRow = ghost->row;
    int currentCol = ghost->col;
    
    int rowDiff = targetRow - currentRow;
    int colDiff = targetCol - currentCol;
    
    switch (ghost->ghostType) {
        case 1:
            if (rowDiff < 0) weights.up *= 3.0f;
            if (rowDiff > 0) weights.down *= 3.0f;
            if (colDiff < 0) weights.left *= 3.0f;
            if (colDiff > 0) weights.right *= 3.0f;
            break;
            
        case 2:
            pthread_mutex_lock(&gameState.mutex);
            Direction pacmanDir = gameState.currentDirection;
            pthread_mutex_unlock(&gameState.mutex);
            
            int aheadRow = targetRow;
            int aheadCol = targetCol;
            
            switch (pacmanDir) {
                case DIR_UP:    aheadRow -= 4; break;
                case DIR_DOWN:  aheadRow += 4; break;
                case DIR_LEFT:  aheadCol -= 4; break;
                case DIR_RIGHT: aheadCol += 4; break;
                default: break;
            }
            
            rowDiff = aheadRow - currentRow;
            colDiff = aheadCol - currentCol;
            
            if (rowDiff < 0) weights.up *= 2.5f;
            if (rowDiff > 0) weights.down *= 2.5f;
            if (colDiff < 0) weights.left *= 2.5f;
            if (colDiff > 0) weights.right *= 2.5f;
            break;
            
        case 3:
            if (rowDiff < 0) weights.up *= 2.0f;
            if (rowDiff > 0) weights.down *= 2.0f;
            if (colDiff < 0) weights.left *= 2.0f;
            if (colDiff > 0) weights.right *= 2.0f;
            
            weights.up *= (1.0f + ((float)rand() / RAND_MAX));
            weights.down *= (1.0f + ((float)rand() / RAND_MAX));
            weights.left *= (1.0f + ((float)rand() / RAND_MAX));
            weights.right *= (1.0f + ((float)rand() / RAND_MAX));
            break;
            
        case 4:
            float distance = sqrt((rowDiff * rowDiff) + (colDiff * colDiff));
            
            if (distance > 8.0f) {
                if (rowDiff < 0) weights.up *= 3.0f;
                if (rowDiff > 0) weights.down *= 3.0f;
                if (colDiff < 0) weights.left *= 3.0f;
                if (colDiff > 0) weights.right *= 3.0f;
            } else {
                int cornerRow = ROWS - 1;
                int cornerCol = 0;
                
                int cornerRowDiff = cornerRow - currentRow;
                int cornerColDiff = cornerCol - currentCol;
                
                if (cornerRowDiff < 0) weights.up *= 2.0f;
                if (cornerRowDiff > 0) weights.down *= 2.0f;
                if (cornerColDiff < 0) weights.left *= 2.0f;
                if (cornerColDiff > 0) weights.right *= 2.0f;
            }
            break;
    }
    
    if (ghost->isVulnerable) {
        float temp = weights.up;
        weights.up = weights.down;
        weights.down = temp;
        
        temp = weights.left;
        weights.left = weights.right;
        weights.right = temp;
    }
    
    switch (ghost->direction) {
        case DIR_UP:    weights.down *= 0.2f; break;
        case DIR_DOWN:  weights.up *= 0.2f; break;
        case DIR_LEFT:  weights.right *= 0.2f; break;
        case DIR_RIGHT: weights.left *= 0.2f; break;
        default: break;
    }
    
    return weights;
}

Direction chooseGhostDirection(Ghost* ghost, DirectionWeights weights) {
    bool validMoves[4] = {false};
    int validCount = 0;
    
    if (isValidGhostMove(ghost->row - 1, ghost->col)) {
        validMoves[DIR_UP - 1] = true;
        validCount++;
    }
    if (isValidGhostMove(ghost->row + 1, ghost->col)) {
        validMoves[DIR_DOWN - 1] = true;
        validCount++;
    }
    if (isValidGhostMove(ghost->row, ghost->col - 1)) {
        validMoves[DIR_LEFT - 1] = true;
        validCount++;
    }
    if (isValidGhostMove(ghost->row, ghost->col + 1)) {
        validMoves[DIR_RIGHT - 1] = true;
        validCount++;
    }
    
    if (!validMoves[DIR_UP - 1]) weights.up = 0.0f;
    if (!validMoves[DIR_DOWN - 1]) weights.down = 0.0f;
    if (!validMoves[DIR_LEFT - 1]) weights.left = 0.0f;
    if (!validMoves[DIR_RIGHT - 1]) weights.right = 0.0f;
    
    float totalWeight = weights.up + weights.down + weights.left + weights.right;
    
    if (totalWeight <= 0.0f || validCount == 0) {
        if (validCount > 0) {
            int randomIndex = rand() % validCount;
            int count = 0;
            for (int i = 0; i < 4; i++) {
                if (validMoves[i]) {
                    if (count == randomIndex) {
                        return (Direction)(i + 1);
                    }
                    count++;
                }
            }
        }
        return DIR_NONE;
    }
    
    float random = ((float)rand() / RAND_MAX) * totalWeight;
    
    if (random < weights.up) return DIR_UP;
    random -= weights.up;
    
    if (random < weights.down) return DIR_DOWN;
    random -= weights.down;
    
    if (random < weights.left) return DIR_LEFT;
    
    return DIR_RIGHT;
}

void moveGhost(Ghost* ghost, Direction direction) {
    if (direction == DIR_NONE) {
        return;
    }
    
    pthread_mutex_lock(&gameState.mutex);
    
    int oldRow = ghost->row;
    int oldCol = ghost->col;
    int newRow = oldRow;
    int newCol = oldCol;
    
    switch (direction) {
        case DIR_UP:    newRow--; break;
        case DIR_DOWN:  newRow++; break;
        case DIR_LEFT:  newCol--; break;
        case DIR_RIGHT: newCol++; break;
        default: break;
    }
    
    if (!isValidGhostMove(newRow, newCol)) {
        pthread_mutex_unlock(&gameState.mutex);
        return;
    }
    
    if (newRow == gameState.pacmanRow && newCol == gameState.pacmanCol) {
        if (ghost->isVulnerable) {
            ghost->needsRespawn = true;
            gameState.score += 200;
            
            gameState.board[oldRow][oldCol] = gameState.originalBoard[oldRow][oldCol];
            
            ghost->row = ghost->respawnRow;
            ghost->col = ghost->respawnCol;
            gameState.board[ghost->row][ghost->col] = '#';
            // New: Ghost is back in ghost house
            ghost->inGhostHouse = true;
            // New: Release resources
            if (ghost->hasKey) {
                sem_post(&keySemaphore);
                ghost->hasKey = false;
            }
            if (ghost->hasExitPermit) {
                sem_post(&exitPermitSemaphore);
                ghost->hasExitPermit = false;
            }
        } else {
            gameState.lives--;
            
            gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
            
            gameState.pacmanRow = gameState.pacmanStartRow;
            gameState.pacmanCol = gameState.pacmanStartCol;
            gameState.board[gameState.pacmanRow][gameState.pacmanCol] = '@';
            
            for (int i = 0; i < MAX_GHOSTS; i++) {
                gameState.board[ghosts[i].row][ghosts[i].col] = ' ';
                ghosts[i].row = ghosts[i].respawnRow;
                ghosts[i].col = ghosts[i].respawnCol;
                ghosts[i].isVulnerable = false;
                gameState.board[ghosts[i].row][ghosts[i].col] = '#';
                // New: Ghosts return to ghost house
                ghosts[i].inGhostHouse = true;
                // New: Release resources
                if (ghosts[i].hasKey) {
                    sem_post(&keySemaphore);
                    ghosts[i].hasKey = false;
                }
                if (ghosts[i].hasExitPermit) {
                    sem_post(&exitPermitSemaphore);
                    ghosts[i].hasExitPermit = false;
                }
            }
            
            gameState.currentDirection = DIR_NONE;
            
            if (gameState.lives <= 0) {
                pthread_mutex_lock(&uiState.mutex);
                uiState.currentScreen = SCREEN_GAME_OVER;
                uiState.needsRedraw = true;
                pthread_mutex_unlock(&uiState.mutex);
            }
        }
        pthread_mutex_unlock(&gameState.mutex);
        return;
    }
    
    gameState.board[oldRow][oldCol] = gameState.originalBoard[oldRow][oldCol];
    
    ghost->row = newRow;
    ghost->col = newCol;
    
    gameState.board[newRow][newCol] = '#';
    
    ghost->direction = direction;
    // New: Ghost has left the ghost house
    ghost->inGhostHouse = false;
    
    pthread_mutex_unlock(&gameState.mutex);
}

typedef struct TimerThreadArgs TimerThreadArgs;
void* ghostTimerThread(void* arg);

typedef struct TimerThreadArgs {
    sem_t* semaphore;
    int intervalMs;
    bool isRunning;
} TimerThreadArgs;

void* ghostThreadFunc(void* arg) {
    Ghost* ghost = (Ghost*)arg;
    
    int moveInterval = 200 + (ghost->id * 50);
    
    sem_t moveSemaphore;
    sem_init(&moveSemaphore, 0, 0);
    
    pthread_t timerThread;
    TimerThreadArgs timerArgs;
    timerArgs.semaphore = &moveSemaphore;
    timerArgs.intervalMs = moveInterval;
    timerArgs.isRunning = true;
    
    pthread_create(&timerThread, NULL, ghostTimerThread, &timerArgs);
    
    while (ghostThreadsRunning) {
        sem_wait(&moveSemaphore);
        
        pthread_mutex_lock(&gameState.mutex);
        bool gameRunning = gameState.gameRunning;
        bool gamePaused = gameState.gamePaused;
        pthread_mutex_unlock(&gameState.mutex);
        
        if (!gameRunning) {
            break;
        }
        
        pthread_mutex_lock(&uiState.mutex);
        bool isPlayScreen = (uiState.currentScreen == SCREEN_PLAY);
        pthread_mutex_unlock(&uiState.mutex);
        
        if (isPlayScreen && !gamePaused) {
            // New: Check if ghost needs to respawn
            if (ghost->needsRespawn) {
                pthread_mutex_lock(&gameState.mutex);
                ghost->row = ghost->respawnRow;
                ghost->col = ghost->respawnCol;
                ghost->isVulnerable = false;
                ghost->needsRespawn = false;
                ghost->inGhostHouse = true; // Back in ghost house
                gameState.board[ghost->row][ghost->col] = '#';
                // Release resources
                if (ghost->hasKey) {
                    sem_post(&keySemaphore);
                    ghost->hasKey = false;
                }
                if (ghost->hasExitPermit) {
                    sem_post(&exitPermitSemaphore);
                    ghost->hasExitPermit = false;
                }
                pthread_mutex_unlock(&gameState.mutex);
            }
            
            // New: Attempt to acquire resources to leave ghost house
            if (ghost->inGhostHouse) {
                bool acquiredResources = false;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1; // 1-second timeout
                
                // Try to acquire key first
                if (!ghost->hasKey && sem_timedwait(&keySemaphore, &ts) == 0) {
                    ghost->hasKey = true;
                    // Then try to acquire exit permit
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_sec += 1;
                    if (!ghost->hasExitPermit && sem_timedwait(&exitPermitSemaphore, &ts) == 0) {
                        ghost->hasExitPermit = true;
                        acquiredResources = true;
                    } else {
                        // Failed to get exit permit, release key
                        sem_post(&keySemaphore);
                        ghost->hasKey = false;
                    }
                }
                
                if (!acquiredResources) {
                    // Couldn't get both resources, wait for next tick
                    continue;
                }
            }
            
            pthread_mutex_lock(&gameState.mutex);
            ghost->isVulnerable = gameState.ghostVulnerable;
            pthread_mutex_unlock(&gameState.mutex);
            
            int pacmanRow, pacmanCol;
            pthread_mutex_lock(&gameState.mutex);
            pacmanRow = gameState.pacmanRow;
            pacmanCol = gameState.pacmanCol;
            pthread_mutex_unlock(&gameState.mutex);
            
            DirectionWeights weights = calculateDirectionWeights(ghost, pacmanRow, pacmanCol);
            
            Direction newDirection = chooseGhostDirection(ghost, weights);
            
            moveGhost(ghost, newDirection);
        }
    }
    
    // New: Release resources before exiting
    if (ghost->hasKey) {
        sem_post(&keySemaphore);
        ghost->hasKey = false;
    }
    if (ghost->hasExitPermit) {
        sem_post(&exitPermitSemaphore);
        ghost->hasExitPermit = false;
    }
    
    timerArgs.isRunning = false;
    pthread_join(timerThread, NULL);
    
    sem_destroy(&moveSemaphore);
    
    pthread_mutex_lock(&ghostExitMutex);
    ghostThreadsExited++;
    
    if (ghostThreadsExited >= MAX_GHOSTS) {
        pthread_cond_signal(&ghostExitCond);
    }
    pthread_mutex_unlock(&ghostExitMutex);
    
    return NULL;
}

void* ghostTimerThread(void* arg) {
    TimerThreadArgs* args = (TimerThreadArgs*)arg;
    
    pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t timerCond = PTHREAD_COND_INITIALIZER;
    
    struct timespec ts;
    
    while (args->isRunning) {
        clock_gettime(CLOCK_REALTIME, &ts);
        
        ts.tv_nsec += (args->intervalMs % 1000) * 1000000;
        ts.tv_sec += args->intervalMs / 1000 + (ts.tv_nsec / 1000000000);
        ts.tv_nsec %= 1000000000;
        
        pthread_mutex_lock(&timerMutex);
        pthread_cond_timedwait(&timerCond, &timerMutex, &ts);  
        pthread_mutex_unlock(&timerMutex);
        
        if (args->isRunning) {
            sem_post(args->semaphore);
        }
    }
    
    pthread_mutex_destroy(&timerMutex);
    pthread_cond_destroy(&timerCond);
    
    return NULL;
}

void startGhostThreads() {
    pthread_mutex_lock(&ghostExitMutex);
    ghostThreadsExited = 0;
    pthread_mutex_unlock(&ghostExitMutex);
    
    for (int i = 0; i < MAX_GHOSTS; i++) {
        if (pthread_create(&ghosts[i].thread, NULL, ghostThreadFunc, &ghosts[i]) != 0) {
            printf("Error creating ghost thread %d\n", i);
        }
    }
}

void stopGhostThreads() {
    ghostThreadsRunning = false;
    
    pthread_mutex_lock(&ghostExitMutex);
    while (ghostThreadsExited < MAX_GHOSTS) {
        pthread_cond_wait(&ghostExitCond, &ghostExitMutex);
    }
    pthread_mutex_unlock(&ghostExitMutex);
}

void cleanupGhostThreadSync() {
    pthread_mutex_destroy(&ghostExitMutex);
    pthread_cond_destroy(&ghostExitCond);
}

void renderGameOver(sfRenderWindow* window, sfFont* font) {
    static bool scoreAdded = false;
    if (!scoreAdded) {
        pthread_mutex_lock(&uiState.mutex);
        addScore(uiState.username, gameState.score);
        pthread_mutex_unlock(&uiState.mutex);
        scoreAdded = true;
    }

    sfRenderWindow_clear(window, sfColor_fromRGB(0, 0, 60));
    
    sfText* titleText = sfText_create();
    sfText_setFont(titleText, font);
    sfText_setString(titleText, "GAME OVER");
    sfText_setCharacterSize(titleText, 72);
    sfText_setFillColor(titleText, sfRed);
    
    sfFloatRect titleBounds = sfText_getLocalBounds(titleText);
    sfText_setPosition(titleText, (sfVector2f){
        (WINDOW_WIDTH - titleBounds.width) / 2,
        WINDOW_HEIGHT * 0.3f
    });
    
    sfRenderWindow_drawText(window, titleText, NULL);
    
    char scoreStr[50];
    pthread_mutex_lock(&gameState.mutex);
    sprintf(scoreStr, "Final Score: %d", gameState.score);
    pthread_mutex_unlock(&gameState.mutex);
    
    sfText* scoreText = sfText_create();
    sfText_setFont(scoreText, font);
    sfText_setString(scoreText, scoreStr);
    sfText_setCharacterSize(scoreText, 36);
    sfText_setFillColor(scoreText, sfWhite);
    
    sfFloatRect scoreBounds = sfText_getLocalBounds(scoreText);
    sfText_setPosition(scoreText, (sfVector2f){
        (WINDOW_WIDTH - scoreBounds.width) / 2,
        WINDOW_HEIGHT * 0.5f
    });
    
    sfRenderWindow_drawText(window, scoreText, NULL);
    
    sfText* instructionsText = sfText_create();
    sfText_setFont(instructionsText, font);
    sfText_setString(instructionsText, "Press ESC to return to menu");
    sfText_setCharacterSize(instructionsText, 24);
    sfText_setFillColor(instructionsText, sfColor_fromRGB(180, 180, 180));
    
    sfFloatRect instrBounds = sfText_getLocalBounds(instructionsText);
    sfText_setPosition(instructionsText, (sfVector2f){
        (WINDOW_WIDTH - instrBounds.width) / 2,
        WINDOW_HEIGHT * 0.7f
    });
    
    sfRenderWindow_drawText(window, instructionsText, NULL);
    
    sfText_destroy(titleText);
    sfText_destroy(scoreText);
    sfText_destroy(instructionsText);
    sfRenderWindow_display(window);
}

void addInputEvent(int eventType, int data) {
    pthread_mutex_lock(&eventQueueMutex);
    if ((eventQueueTail + 1) % MAX_INPUT_EVENTS != eventQueueHead) {
        inputEventQueue[eventQueueTail].eventType = eventType;
        inputEventQueue[eventQueueTail].data = data;
        inputEventQueue[eventQueueTail].processed = false;
        eventQueueTail = (eventQueueTail + 1) % MAX_INPUT_EVENTS;
    }
    pthread_mutex_unlock(&eventQueueMutex);
}

bool getNextInputEvent(InputEvent* event) {
    bool hasEvent = false;
    pthread_mutex_lock(&eventQueueMutex);
    if (eventQueueHead != eventQueueTail) {
        *event = inputEventQueue[eventQueueHead];
        inputEventQueue[eventQueueHead].processed = true;
        eventQueueHead = (eventQueueHead + 1) % MAX_INPUT_EVENTS;
        hasEvent = true;
    }
    pthread_mutex_unlock(&eventQueueMutex);
    return hasEvent;
}

void initUIState() {
    uiState.currentScreen = SCREEN_MENU;
    uiState.selectedMenuItem = 0;
    uiState.needsRedraw = true;
    strcpy(uiState.username, "Unknown");
    uiState.enteringUsername = false;
    uiState.usernameCursorPos = 0;
    pthread_mutex_init(&uiState.mutex, NULL);
}

void initGameState() {
    gameState.powerPelletActive = false; 
    gameState.powerPelletDuration = 0.0f;
    gameState.ghostVulnerable = false;
    gameState.ghostVulnerableDuration = 0.0f;
    
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            gameState.board[i][j] = initialBoard[i][j];
        }
    }
    
    gameState.score = 0;
    gameState.lives = 3;
    gameState.pacmanRotation = 0.0f;
    gameState.currentDirection = DIR_NONE;
    gameState.gameRunning = true;
    gameState.gamePaused = false;
    
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            if (gameState.board[i][j] == '@') {
                gameState.pacmanStartRow = i;
                gameState.pacmanStartCol = j;
                gameState.pacmanRow = i;
                gameState.pacmanCol = j;
                break;
            }
        }
    }
    saveOriginalBoard();
    initGhosts();
    pthread_mutex_init(&gameState.mutex, NULL);
}

void movePacman() {
    pthread_mutex_lock(&gameState.mutex);
    
    switch(gameState.currentDirection) {
        case DIR_UP:
            if (gameState.pacmanRow > 0 && gameState.board[gameState.pacmanRow-1][gameState.pacmanCol] != '=') {
                if(gameState.board[gameState.pacmanRow-1][gameState.pacmanCol] == '.') {
                    gameState.score += 10;
                }
                else if(gameState.board[gameState.pacmanRow-1][gameState.pacmanCol] == '0') {
                    gameState.score += 50;
                    gameState.powerPelletActive = true;
                    gameState.ghostVulnerable = true; 
                    gameState.powerPelletDuration = 0.0f;
                    gameState.ghostVulnerableDuration = 0.0f;
                }
                else if(gameState.board[gameState.pacmanRow-1][gameState.pacmanCol] == '#') {
                    gameState.ghostVulnerable = true;
                    gameState.ghostVulnerableDuration = 0.0f;
                    gameState.score += 200;
                }
                
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
                gameState.pacmanRow--;
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = '@';
            }
            gameState.pacmanRotation = 90.0f;
            break;
        case DIR_DOWN:
            if (gameState.pacmanRow < ROWS-1 && gameState.board[gameState.pacmanRow+1][gameState.pacmanCol] != '=') {
                if(gameState.board[gameState.pacmanRow+1][gameState.pacmanCol] == '.') {
                    gameState.score += 10;
                }
                else if(gameState.board[gameState.pacmanRow+1][gameState.pacmanCol] == '0') {
                    gameState.score += 50; 
                    gameState.powerPelletActive = true;
                    gameState.ghostVulnerable = true;
                    gameState.powerPelletDuration = 0.0f;
                    gameState.ghostVulnerableDuration = 0.0f;
                }
                else if(gameState.board[gameState.pacmanRow+1][gameState.pacmanCol] == '#') {
                    gameState.ghostVulnerable = true;
                    gameState.ghostVulnerableDuration = 0.0f;
                    gameState.score += 200; 
                }
                
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
                gameState.pacmanRow++;
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = '@';
            }
            gameState.pacmanRotation = 270.0f;
            break;
        case DIR_LEFT:
            if (gameState.pacmanCol > 0 && gameState.board[gameState.pacmanRow][gameState.pacmanCol-1] != '=') {
                if(gameState.board[gameState.pacmanRow][gameState.pacmanCol-1] == '.') {
                    gameState.score += 10; 
                }
                else if(gameState.board[gameState.pacmanRow][gameState.pacmanCol-1] == '0') {
                    gameState.score += 50; 
                    gameState.powerPelletActive = true;
                    gameState.ghostVulnerable = true;
                    gameState.powerPelletDuration = 0.0f;
                    gameState.ghostVulnerableDuration = 0.0f;
                }
                else if(gameState.board[gameState.pacmanRow][gameState.pacmanCol-1] == '#') {
                    gameState.ghostVulnerable = true;
                    gameState.ghostVulnerableDuration = 0.0f;
                    gameState.score += 200;
                }
                
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
                gameState.pacmanCol--;
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = '@';
            }
            gameState.pacmanRotation = 0.0f;
            break;
        case DIR_RIGHT:
            if (gameState.pacmanCol < COLS-1 && gameState.board[gameState.pacmanRow][gameState.pacmanCol+1] != '=') {
                if(gameState.board[gameState.pacmanRow][gameState.pacmanCol+1] == '.') {
                    gameState.score += 10;
                }
                else if (gameState.board[gameState.pacmanRow][gameState.pacmanCol+1] == '0') {
                    gameState.score += 50; 
                    gameState.powerPelletActive = true;
                    gameState.ghostVulnerable = true;
                    gameState.powerPelletDuration = 0.0f;
                    gameState.ghostVulnerableDuration = 0.0f;
                }
                else if(gameState.board[gameState.pacmanRow][gameState.pacmanCol+1] == '#') {
                    gameState.ghostVulnerable = true;
                    gameState.ghostVulnerableDuration = 0.0f;
                    gameState.score += 200;
                }
                
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
                gameState.pacmanCol++;
                gameState.board[gameState.pacmanRow][gameState.pacmanCol] = '@';
            }
            gameState.pacmanRotation = 180.0f;
            break;
        case DIR_NONE:
            break;
    }
    
    if (gameState.powerPelletActive) {
        if (gameState.board[gameState.pacmanRow][gameState.pacmanCol] == '#') {
            gameState.score += 200;
            gameState.board[gameState.pacmanRow][gameState.pacmanCol] = ' ';
        }
    }
    pthread_mutex_unlock(&gameState.mutex);
}

void renderMenu(sfRenderWindow* window, sfFont* font) {
    sfRenderWindow_clear(window, sfColor_fromRGB(0, 0, 60));
    
    sfText* titleText = sfText_create();
    sfText_setFont(titleText, font);
    sfText_setString(titleText, "PACMAN");
    sfText_setCharacterSize(titleText, 72);
    sfText_setFillColor(titleText, sfYellow);
    
    sfFloatRect titleBounds = sfText_getLocalBounds(titleText);
    sfText_setPosition(titleText, (sfVector2f){
        (WINDOW_WIDTH - titleBounds.width) / 2,
        WINDOW_HEIGHT * 0.2f
    });
    
    sfRenderWindow_drawText(window, titleText, NULL);

    pthread_mutex_lock(&uiState.mutex);
    char usernameLabel[64];
    sprintf(usernameLabel, "Player: %s", uiState.username);
    bool enteringUsername = uiState.enteringUsername;
    int selectedItem = uiState.selectedMenuItem;
    pthread_mutex_unlock(&uiState.mutex);

    sfText* usernameText = sfText_create();
    sfText_setFont(usernameText, font);
    sfText_setString(usernameText, usernameLabel);
    sfText_setCharacterSize(usernameText, 28);
    sfText_setFillColor(usernameText, enteringUsername ? sfYellow : sfWhite);
    
    sfFloatRect usernameBounds = sfText_getLocalBounds(usernameText);
    sfText_setPosition(usernameText, (sfVector2f){
        (WINDOW_WIDTH - usernameBounds.width) / 2,
        WINDOW_HEIGHT * 0.35f
    });
    
    sfRenderWindow_drawText(window, usernameText, NULL);
    sfText_destroy(usernameText);

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        sfText* menuItemText = sfText_create();
        sfText_setFont(menuItemText, font);
        sfText_setString(menuItemText, menuItems[i]);
        sfText_setCharacterSize(menuItemText, 36);
        
        if (i == selectedItem) {
            sfText_setFillColor(menuItemText, sfYellow);
            char selectedText[50];
            sprintf(selectedText, "> %s", menuItems[i]);
            sfText_setString(menuItemText, selectedText);
        } else {
            sfText_setFillColor(menuItemText, sfWhite);
        }
        
        sfFloatRect itemBounds = sfText_getLocalBounds(menuItemText);
        sfText_setPosition(menuItemText, (sfVector2f){
            (WINDOW_WIDTH - itemBounds.width) / 2,
            WINDOW_HEIGHT * 0.4f + i * 60
        });
        
        sfRenderWindow_drawText(window, menuItemText, NULL);
        sfText_destroy(menuItemText);
    }
    
    sfText* instructionsText = sfText_create();
    sfText_setFont(instructionsText, font);
    sfText_setString(instructionsText, 
        "Press U to set username | UP/DOWN to navigate | ENTER to select");
    sfText_setCharacterSize(instructionsText, 20);
    sfText_setFillColor(instructionsText, sfColor_fromRGB(180, 180, 180));
    
    sfFloatRect instrBounds = sfText_getLocalBounds(instructionsText);
    sfText_setPosition(instructionsText, (sfVector2f){
        (WINDOW_WIDTH - instrBounds.width) / 2,
        WINDOW_HEIGHT * 0.85f
    });
    
    sfRenderWindow_drawText(window, instructionsText, NULL);
    
    sfText_destroy(titleText);
    sfText_destroy(instructionsText);
    sfRenderWindow_display(window);
}

void renderScoreboard(sfRenderWindow* window, sfFont* font) {
    sfRenderWindow_clear(window, sfColor_fromRGB(0, 0, 60));
    
    sfText* titleText = sfText_create();
    sfText_setFont(titleText, font);
    sfText_setString(titleText, "SCOREBOARD");
    sfText_setCharacterSize(titleText, 48);
    sfText_setFillColor(titleText, sfYellow);
    
    sfFloatRect titleBounds = sfText_getLocalBounds(titleText);
    sfText_setPosition(titleText, (sfVector2f){
        (WINDOW_WIDTH - titleBounds.width) / 2,
        WINDOW_HEIGHT * 0.2f
    });
    
    sfRenderWindow_drawText(window, titleText, NULL);
    
    for (int i = 0; i < scoreCount; i++) {
        char scoreLine[64];
        sprintf(scoreLine, "%d. %s - %d", i + 1, scoreBoard[i].username, scoreBoard[i].score);
        
        sfText* scoreText = sfText_create();
        sfText_setFont(scoreText, font);
        sfText_setString(scoreText, scoreLine);
        sfText_setCharacterSize(scoreText, 24);
        sfText_setFillColor(scoreText, sfWhite);
        
        sfFloatRect scoreBounds = sfText_getLocalBounds(scoreText);
        sfText_setPosition(scoreText, (sfVector2f){
            (WINDOW_WIDTH - scoreBounds.width) / 2,
            WINDOW_HEIGHT * 0.3f + i * 30
        });
        
        sfRenderWindow_drawText(window, scoreText, NULL);
        sfText_destroy(scoreText);
    }
    
    sfText* instructionsText = sfText_create();
    sfText_setFont(instructionsText, font);
    sfText_setString(instructionsText, "Press ESC to return to menu");
    sfText_setCharacterSize(instructionsText, 20);
    sfText_setFillColor(instructionsText, sfColor_fromRGB(180, 180, 180));
    
    sfFloatRect instrBounds = sfText_getLocalBounds(instructionsText);
    sfText_setPosition(instructionsText, (sfVector2f){
        (WINDOW_WIDTH - instrBounds.width) / 2,
        WINDOW_HEIGHT * 0.85f
    });
    
    sfRenderWindow_drawText(window, instructionsText, NULL);
    
    sfText_destroy(titleText);
    sfText_destroy(instructionsText);
    sfRenderWindow_display(window);
}

void renderInstructions(sfRenderWindow* window, sfFont* font) {
    sfRenderWindow_clear(window, sfColor_fromRGB(0, 0, 60));
    
    sfText* titleText = sfText_create();
    sfText_setFont(titleText, font);
    sfText_setString(titleText, "INSTRUCTIONS");
    sfText_setCharacterSize(titleText, 48);
    sfText_setFillColor(titleText, sfYellow);
    
    sfFloatRect titleBounds = sfText_getLocalBounds(titleText);
    sfText_setPosition(titleText, (sfVector2f){
        (WINDOW_WIDTH - titleBounds.width) / 2,
        WINDOW_HEIGHT * 0.1f
    });
    
    sfRenderWindow_drawText(window, titleText, NULL);
    
    const char* instructionsLines[] = {
        "Use the WASD keys to control Pacman:",
        "W - Move Up",
        "A - Move Left",
        "S - Move Down",
        "D - Move Right",
        "",
        "Collect dots for 10 points each",
        "Power pellets (large dots) worth 50 points",
        "Eat ghosts for 200 points when powered up",
        "",
        "Press ESC to return to menu during gameplay"
    };
    
    for (int i = 0; i < 11; i++) {
        sfText* lineText = sfText_create();
        sfText_setFont(lineText, font);
        sfText_setString(lineText, instructionsLines[i]);
        sfText_setCharacterSize(lineText, 24);
        sfText_setFillColor(lineText, sfWhite);
        
        sfText_setPosition(lineText, (sfVector2f){
            WINDOW_WIDTH * 0.15f,
            WINDOW_HEIGHT * 0.25f + i * 35
        });
        
        sfRenderWindow_drawText(window, lineText, NULL);
        sfText_destroy(lineText);
    }
    
    sfText* backText = sfText_create();
    sfText_setFont(backText, font);
    sfText_setString(backText, "Press ESC to return to menu");
    sfText_setCharacterSize(backText, 20);
    sfText_setFillColor(backText, sfColor_fromRGB(180, 180, 180));
    
    sfFloatRect backBounds = sfText_getLocalBounds(backText);
    sfText_setPosition(backText, (sfVector2f){
        (WINDOW_WIDTH - backBounds.width) / 2,
        WINDOW_HEIGHT * 0.9f
    });
    
    sfRenderWindow_drawText(window, backText, NULL);
    
    sfText_destroy(titleText);
    sfText_destroy(backText);
    sfRenderWindow_display(window);
}

void renderGame(sfRenderWindow* window, sfRectangleShape* wall, sfCircleShape* dot, 
               sfCircleShape* powerPellet, sfSprite* pacmanSprite, sfSprite* ghost1Sprite,
               sfSprite* ghost2Sprite, sfSprite* ghost3Sprite, sfSprite* ghost4Sprite,
               sfSprite* ghost5Sprite, sfText* scoreText, sfText* livesText,
               sfSprite* lifeSprite) 
{
    sfRenderWindow_clear(window, sfBlack);
    pthread_mutex_lock(&gameState.mutex);
    
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            float x = j * CELL_SIZE;
            float y = i * CELL_SIZE;
            
            switch(gameState.board[i][j]) {
                case '=':
                    sfRectangleShape_setPosition(wall, (sfVector2f){x, y});
                    sfRenderWindow_drawRectangleShape(window, wall, NULL);
                    break;
                case '.':
                    sfCircleShape_setPosition(dot, (sfVector2f){x + CELL_SIZE/2 - 3, y + CELL_SIZE/2 - 3});
                    sfRenderWindow_drawCircleShape(window, dot, NULL);
                    break;
                case '0':
                    if (pelletVisible) {
                        sfCircleShape_setPosition(powerPellet, (sfVector2f){x + CELL_SIZE/2 - 13, y + CELL_SIZE/2 - 13});
                        sfRenderWindow_drawCircleShape(window, powerPellet, NULL);
                    }
                    break;
                case '@':
                    sfSprite_setPosition(pacmanSprite, (sfVector2f){x + CELL_SIZE/2, y + CELL_SIZE/2});
                    sfSprite_setRotation(pacmanSprite, gameState.pacmanRotation);
                    sfRenderWindow_drawSprite(window, pacmanSprite, NULL);
                    break;
                case '#':
                    for (int g = 0; g < MAX_GHOSTS; g++) {
                        if (ghosts[g].row == i && ghosts[g].col == j) {
                            sfSprite* currentGhostSprite;
                            
                            if (ghosts[g].isVulnerable) {
                                currentGhostSprite = ghost5Sprite;
                            } else {
                                switch (ghosts[g].ghostType) {
                                    case 1: currentGhostSprite = ghost1Sprite; break;
                                    case 2: currentGhostSprite = ghost2Sprite; break;
                                    case 3: currentGhostSprite = ghost3Sprite; break;
                                    case 4: currentGhostSprite = ghost4Sprite; break;
                                    default: currentGhostSprite = ghost1Sprite;
                                }
                            }
                            
                            sfSprite_setPosition(currentGhostSprite, (sfVector2f){x + CELL_SIZE/2, y + CELL_SIZE/2});
                            sfRenderWindow_drawSprite(window, currentGhostSprite, NULL);
                            break;
                        }
                    }
                    break;
            }
        }
    }
    
    char scoreStr[50];
    sprintf(scoreStr, "Score: %d", gameState.score);
    sfText_setString(scoreText, scoreStr);
    sfText_setPosition(scoreText, (sfVector2f){10 * CELL_SIZE + CELL_SIZE / 4.0, 19 * CELL_SIZE + CELL_SIZE / 4.0f});
    sfRenderWindow_drawText(window, scoreText, NULL);
    
    float lifeY = 19 * CELL_SIZE + CELL_SIZE / 4.0f;
    float lifeX = 7 * CELL_SIZE + CELL_SIZE / 4.0f;
    for (int i = 0; i < gameState.lives; i++) {
        sfSprite_setPosition(lifeSprite, (sfVector2f){lifeX - i * (CELL_SIZE / 2), lifeY});
        sfRenderWindow_drawSprite(window, lifeSprite, NULL);
    }
    
    pthread_mutex_unlock(&gameState.mutex);
    sfRenderWindow_display(window);
}

void processInput(sfRenderWindow* window) {
    sfEvent event;
    while (sfRenderWindow_pollEvent(window, &event)) {
        if (event.type == sfEvtClosed) {
            pthread_mutex_lock(&gameState.mutex);
            gameState.gameRunning = false;
            pthread_mutex_unlock(&gameState.mutex);
            sfRenderWindow_close(window);
        }
        else if (event.type == sfEvtKeyPressed) {
            pthread_mutex_lock(&uiState.mutex);
            GameScreen currentScreen = uiState.currentScreen;
            bool enteringUsername = uiState.enteringUsername;
            pthread_mutex_unlock(&uiState.mutex);
            
            if (currentScreen == SCREEN_MENU) {
                if (enteringUsername) {
                    if (event.key.code == sfKeyEnter || event.key.code == sfKeyEscape) {
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.enteringUsername = false;
                        uiState.needsRedraw = true;
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                    else if (event.key.code == sfKeyBackspace) {
                        pthread_mutex_lock(&uiState.mutex);
                        if (uiState.usernameCursorPos > 0) {
                            uiState.username[--uiState.usernameCursorPos] = '\0';
                            uiState.needsRedraw = true;
                        }
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                }
                else {
                    if (event.key.code == sfKeyU) {
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.enteringUsername = true;
                        uiState.username[0] = '\0';
                        uiState.usernameCursorPos = 0;
                        uiState.needsRedraw = true;
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                    else if (event.key.code == sfKeyUp) {
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.selectedMenuItem = (uiState.selectedMenuItem - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
                        uiState.needsRedraw = true;
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                    else if (event.key.code == sfKeyDown) {
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.selectedMenuItem = (uiState.selectedMenuItem + 1) % MENU_ITEM_COUNT;
                        uiState.needsRedraw = true;
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                    else if (event.key.code == sfKeyReturn) {
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.needsRedraw = true;
                        
                        switch (uiState.selectedMenuItem) {
                            case 0:
                                uiState.currentScreen = SCREEN_PLAY;
                                addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_PLAY);
                                break;
                            case 1:
                                uiState.currentScreen = SCREEN_SCOREBOARD;
                                addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_SCOREBOARD);
                                break;
                            case 2:
                                uiState.currentScreen = SCREEN_INSTRUCTIONS;
                                addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_INSTRUCTIONS);
                                break;
                            case 3:
                                uiState.currentScreen = SCREEN_QUIT;
                                addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_QUIT);
                                gameState.gameRunning = false;
                                break;
                        }
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                }
            }
            else if (currentScreen == SCREEN_PLAY) {
                if (event.key.code == sfKeyEscape) {
                    pthread_mutex_lock(&uiState.mutex);
                    uiState.currentScreen = SCREEN_MENU;
                    uiState.needsRedraw = true;
                    pthread_mutex_unlock(&uiState.mutex);
                    addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_MENU);
                }
                else if (event.key.code == sfKeyW || event.key.code == sfKeyUp) {
                    pthread_mutex_lock(&gameState.mutex);
                    gameState.currentDirection = DIR_UP;
                    gameState.pacmanRotation = 270.0f;
                    pthread_mutex_unlock(&gameState.mutex);
                    addInputEvent(EVENT_DIRECTION_CHANGE, DIR_UP);
                }
                else if (event.key.code == sfKeyS || event.key.code == sfKeyDown) {
                    pthread_mutex_lock(&gameState.mutex);
                    gameState.currentDirection = DIR_DOWN;
                    gameState.pacmanRotation = 90.0f;
                    pthread_mutex_unlock(&gameState.mutex);
                    addInputEvent(EVENT_DIRECTION_CHANGE, DIR_DOWN);
                }
                else if (event.key.code == sfKeyA || event.key.code == sfKeyLeft) {
                    pthread_mutex_lock(&gameState.mutex);
                    gameState.currentDirection = DIR_LEFT; 
                    gameState.pacmanRotation = 180.0f;
                    pthread_mutex_unlock(&gameState.mutex);
                    addInputEvent(EVENT_DIRECTION_CHANGE, DIR_LEFT);
                }
                else if (event.key.code == sfKeyD || event.key.code == sfKeyRight) {
                    pthread_mutex_lock(&gameState.mutex);
                    gameState.currentDirection = DIR_RIGHT;
                    gameState.pacmanRotation = 0.0f;
                    pthread_mutex_unlock(&gameState.mutex);
                    addInputEvent(EVENT_DIRECTION_CHANGE, DIR_RIGHT);
                }
                else if (event.key.code == sfKeyP) {
                    pthread_mutex_lock(&gameState.mutex);
                    gameState.gamePaused = !gameState.gamePaused;
                    pthread_mutex_unlock(&gameState.mutex);
                }
            }
            else if (currentScreen == SCREEN_SCOREBOARD || currentScreen == SCREEN_INSTRUCTIONS || currentScreen == SCREEN_GAME_OVER) {
                if (event.key.code == sfKeyEscape) {
                    pthread_mutex_lock(&uiState.mutex);
                    uiState.currentScreen = SCREEN_MENU;
                    uiState.needsRedraw = true;
                    pthread_mutex_unlock(&uiState.mutex);
                    addInputEvent(EVENT_SCREEN_CHANGE, SCREEN_MENU);
                }
            }
        }
        else if (event.type == sfEvtTextEntered) {
            pthread_mutex_lock(&uiState.mutex);
            bool enteringUsername = uiState.enteringUsername;
            pthread_mutex_unlock(&uiState.mutex);
            
            if (enteringUsername && event.text.unicode < 128 && event.text.unicode != '\r' && event.text.unicode != '\n') {
                pthread_mutex_lock(&uiState.mutex);
                if (event.text.unicode == '\b') {
                }
                else if (uiState.usernameCursorPos < sizeof(uiState.username) - 1) {
                    uiState.username[uiState.usernameCursorPos++] = (char)event.text.unicode;
                    uiState.username[uiState.usernameCursorPos] = '\0';
                    uiState.needsRedraw = true;
                }
                pthread_mutex_unlock(&uiState.mutex);
            }
        }
    }
}

void* gameTickTimerThread(void* arg) {
    TimerThreadArgs* args = (TimerThreadArgs*)arg;
    
    pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t timerCond = PTHREAD_COND_INITIALIZER;
    
    struct timespec ts;
    
    while (args->isRunning) {
        clock_gettime(CLOCK_REALTIME, &ts);
        
        ts.tv_nsec += (args->intervalMs % 1000) * 1000000;
        ts.tv_sec += args->intervalMs / 1000 + (ts.tv_nsec / 1000000000);
        ts.tv_nsec %= 1000000000;
        
        pthread_mutex_lock(&timerMutex);
        pthread_cond_timedwait(&timerCond, &timerMutex, &ts);  
        pthread_mutex_unlock(&timerMutex);
        
        if (args->isRunning) {
            sem_post(args->semaphore);
        }
    }
    
    pthread_mutex_destroy(&timerMutex);
    pthread_cond_destroy(&timerCond);
    
    return NULL;
}

void* gameEngineThreadFunc(void* arg) {
    sem_t gameTick;
    sem_init(&gameTick, 0, 0);
    
    pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t frameCond = PTHREAD_COND_INITIALIZER;
    
    pthread_t tickThread;
    TimerThreadArgs tickArgs;
    tickArgs.semaphore = &gameTick;
    tickArgs.intervalMs = 200;
    tickArgs.isRunning = true;
    
    pthread_create(&tickThread, NULL, gameTickTimerThread, &tickArgs);
    
    while (true) {
        sem_wait(&gameTick);
        
        pthread_mutex_lock(&gameState.mutex);
        bool gameRunning = gameState.gameRunning;
        bool gamePaused = gameState.gamePaused;
        float deltaTime = 0.2f;
        pthread_mutex_unlock(&gameState.mutex);
        
        if (!gameRunning) {
            break;
        }
        
        InputEvent event;
        while (getNextInputEvent(&event)) {
            if (event.eventType == EVENT_DIRECTION_CHANGE) {
                pthread_mutex_lock(&gameState.mutex);
                gameState.currentDirection = event.data;
                switch(event.data) {
                    case DIR_UP: gameState.pacmanRotation = 270.0f; break;
                    case DIR_DOWN: gameState.pacmanRotation = 90.0f; break;
                    case DIR_LEFT: gameState.pacmanRotation = 180.0f; break;
                    case DIR_RIGHT: gameState.pacmanRotation = 0.0f; break;
                    default: break;
                }
                pthread_mutex_unlock(&gameState.mutex);
            }
            else if (event.eventType == EVENT_SCREEN_CHANGE) {
                if (event.data == SCREEN_PLAY) {
                    initGameState();
                }
            }
        }
        
        pthread_mutex_lock(&uiState.mutex);
        bool isPlayScreen = (uiState.currentScreen == SCREEN_PLAY);
        pthread_mutex_unlock(&uiState.mutex);
        
        if (isPlayScreen && !gamePaused) {
            movePacman();
            
            pthread_mutex_lock(&frameMutex);
            pthread_cond_broadcast(&frameCond);
            pthread_mutex_unlock(&frameMutex);
            
            pthread_mutex_lock(&gameState.mutex);
            
            if (gameState.powerPelletActive) {
                gameState.powerPelletDuration += deltaTime;
                if (gameState.powerPelletDuration >= 10.0f) {
                    gameState.powerPelletActive = false;
                    gameState.ghostVulnerable = false;
                }
            }
            
            if (gameState.ghostVulnerable) {
                gameState.ghostVulnerableDuration += deltaTime;
                
                if (gameState.ghostVulnerableDuration >= 6.0f) {
                    gameState.ghostVulnerable = false;
                }
            }
            
            pthread_mutex_unlock(&gameState.mutex);
        }
    }
    
    tickArgs.isRunning = false;
    pthread_join(tickThread, NULL);
    
    sem_destroy(&gameTick);
    pthread_mutex_destroy(&frameMutex);
    pthread_cond_destroy(&frameCond);
    
    pthread_mutex_lock(&gameEngineThreadExitMutex);
    gameEngineThreadExited = true;
    pthread_cond_signal(&gameEngineThreadExitCond);
    pthread_mutex_unlock(&gameEngineThreadExitMutex);
    
    return NULL;
}

int main() {
    // New: Initialize resource semaphores
    sem_init(&keySemaphore, 0, MAX_KEYS);
    sem_init(&exitPermitSemaphore, 0, MAX_EXIT_PERMITS);
    
    loadScores();
    sfVideoMode mode = {WINDOW_WIDTH, WINDOW_HEIGHT, 32};
    sfRenderWindow* window = sfRenderWindow_create(mode, "Pacman", sfClose, NULL);
    if (!window) {
        return -1;
    }
    
    sfRenderWindow_setFramerateLimit(window, 60);
    
    sfFont* font = sfFont_createFromFile("ARIAL.TTF");
    if (!font) {
        printf("Error loading font\n");
        return -1;
    }
    
    sfRectangleShape* wall = sfRectangleShape_create();
    sfRectangleShape_setSize(wall, (sfVector2f){CELL_SIZE, CELL_SIZE});
    sfRectangleShape_setFillColor(wall, sfColor_fromRGB(33, 33, 222));
    
    sfCircleShape* dot = sfCircleShape_create();
    sfCircleShape_setRadius(dot, 3);
    sfCircleShape_setFillColor(dot, sfColor_fromRGB(255, 184, 174));
    
    sfCircleShape* powerPellet = sfCircleShape_create();
    sfCircleShape_setRadius(powerPellet, 12);
    sfCircleShape_setFillColor(powerPellet, sfColor_fromRGB(255, 184, 174));
    
    ghost1Texture = sfTexture_createFromFile("ghost.jpeg", NULL);
    ghost5Texture = sfTexture_createFromFile("ghostDed.jpg", NULL);
    sfTexture* pacmanTexture = sfTexture_createFromFile("pacman1.jpeg", NULL);
    ghost2Texture = sfTexture_createFromFile("ghost2.jpeg", NULL);
    ghost3Texture = sfTexture_createFromFile("ghost3.jpeg", NULL);
    ghost4Texture = sfTexture_createFromFile("ghost4.jpeg", NULL);
    sfTexture* lifeTexture = sfTexture_createFromFile("live.png", NULL);
    
    sfSprite* pacmanSprite = sfSprite_create();
    sfSprite_setTexture(pacmanSprite, pacmanTexture, sfTrue);
    sfSprite_setOrigin(pacmanSprite, (sfVector2f){25, 25});
    
    sfSprite* ghost1Sprite = sfSprite_create();
    sfSprite_setTexture(ghost1Sprite, ghost1Texture, sfTrue);
    sfSprite_setOrigin(ghost1Sprite, (sfVector2f){25, 25});
    
    sfSprite* ghost2Sprite = sfSprite_create();
    sfSprite_setTexture(ghost2Sprite, ghost2Texture, sfTrue);
    sfSprite_setOrigin(ghost2Sprite, (sfVector2f){25, 25});
    
    sfSprite* ghost3Sprite = sfSprite_create();
    sfSprite_setTexture(ghost3Sprite, ghost3Texture, sfTrue);
    sfSprite_setOrigin(ghost3Sprite, (sfVector2f){25, 25});
    
    sfSprite* ghost4Sprite = sfSprite_create();
    sfSprite_setTexture(ghost4Sprite, ghost4Texture, sfTrue);
    sfSprite_setOrigin(ghost4Sprite, (sfVector2f){25, 25});
    
    sfSprite* ghost5Sprite = sfSprite_create();
    sfSprite_setTexture(ghost5Sprite, ghost5Texture, sfTrue);
    sfSprite_setOrigin(ghost5Sprite, (sfVector2f){25, 25});
    
    sfSprite* lifeSprite = sfSprite_create();
    sfSprite_setTexture(lifeSprite, lifeTexture, sfTrue);
    sfSprite_setScale(lifeSprite, (sfVector2f){0.5, 0.5});
    sfSprite_setOrigin(lifeSprite, (sfVector2f){25, 25});
    
    sfText* scoreText = sfText_create();
    sfText_setFont(scoreText, font);
    sfText_setCharacterSize(scoreText, 24);
    sfText_setFillColor(scoreText, sfWhite);
    
    sfText* livesText = sfText_create();
    sfText_setFont(livesText, font);
    sfText_setCharacterSize(livesText, 24);
    sfText_setFillColor(livesText, sfWhite);
    sfText_setString(livesText, "Lives:");
    
    initUIState();
    static bool scoreAdded = false;
    scoreAdded = false;
    initGameState();
    
    gameClock = sfClock_create();
    pelletBlinkClock = sfClock_create();
    
    pthread_t gameEngineThread;
    if (pthread_create(&gameEngineThread, NULL, gameEngineThreadFunc, NULL) != 0) {
        printf("Error creating game engine thread\n");
        return -1;
    }
    
    startGhostThreads();
    
    while (sfRenderWindow_isOpen(window)) {
        processInput(window);
        
        pthread_mutex_lock(&uiState.mutex);
        GameScreen currentScreen = uiState.currentScreen;
        bool needsRedraw = uiState.needsRedraw;
        uiState.needsRedraw = false;
        pthread_mutex_unlock(&uiState.mutex);
        
        pthread_mutex_lock(&gameState.mutex);
        bool gameRunning = gameState.gameRunning;
        pthread_mutex_unlock(&gameState.mutex);
        
        if (!gameRunning) {
            sfRenderWindow_close(window);
            break;
        }
        
        InputEvent event;
        while (getNextInputEvent(&event)) {
            if (event.eventType == EVENT_SCREEN_CHANGE && event.data == SCREEN_PLAY) {
                scoreAdded = false;
                initGameState();
            }
        }
        
        sfTime elapsed = sfClock_getElapsedTime(pelletBlinkClock);
        if (sfTime_asSeconds(elapsed) >= pelletBlinkInterval) {
            pelletVisible = !pelletVisible;
            sfClock_restart(pelletBlinkClock);
        }
        
        switch (currentScreen) {
            case SCREEN_MENU:
                renderMenu(window, font);
                break;
            case SCREEN_PLAY:
                renderGame(window, wall, dot, powerPellet, pacmanSprite, ghost1Sprite,
                          ghost2Sprite, ghost3Sprite, ghost4Sprite, ghost5Sprite,
                          scoreText, livesText, lifeSprite);
                break;
            case SCREEN_SCOREBOARD:
                renderScoreboard(window, font);
                break;
            case SCREEN_INSTRUCTIONS:
                renderInstructions(window, font);
                break;
            case SCREEN_GAME_OVER:
                renderGameOver(window, font);
                break;
            case SCREEN_QUIT:
                sfRenderWindow_close(window);
                break;
        }
    }
    
    pthread_mutex_lock(&gameState.mutex);
    gameState.gameRunning = false;
    pthread_mutex_unlock(&gameState.mutex);
    
    pthread_mutex_lock(&gameEngineThreadExitMutex);
    while (!gameEngineThreadExited) {
        pthread_cond_wait(&gameEngineThreadExitCond, &gameEngineThreadExitMutex);
    }
    pthread_mutex_unlock(&gameEngineThreadExitMutex);
    
    pthread_mutex_lock(&ghostExitMutex);
    ghostThreadsRunning = false;
    while (ghostThreadsExited < MAX_GHOSTS) {
        pthread_cond_wait(&ghostExitCond, &ghostExitMutex);
    }
    pthread_mutex_unlock(&ghostExitMutex);
    
    sfClock_destroy(gameClock);
    sfClock_destroy(pelletBlinkClock);
    
    sfRectangleShape_destroy(wall);
    sfCircleShape_destroy(dot);
    sfCircleShape_destroy(powerPellet);
    
    sfSprite_destroy(pacmanSprite);
    sfSprite_destroy(ghost1Sprite);
    sfSprite_destroy(ghost2Sprite);
    sfSprite_destroy(ghost3Sprite);
    sfSprite_destroy(ghost4Sprite);
    sfSprite_destroy(ghost5Sprite);
    sfSprite_destroy(lifeSprite);
    
    sfTexture_destroy(pacmanTexture);
    sfTexture_destroy(ghost1Texture);
    sfTexture_destroy(ghost2Texture);
    sfTexture_destroy(ghost3Texture);
    sfTexture_destroy(ghost4Texture);
    sfTexture_destroy(ghost5Texture);
    sfTexture_destroy(lifeTexture);
    
    sfText_destroy(scoreText);
    sfText_destroy(livesText);
    
    sfFont_destroy(font);
    sfRenderWindow_destroy(window);
    
    // New: Destroy resource semaphores
    sem_destroy(&keySemaphore);
    sem_destroy(&exitPermitSemaphore);
    
    pthread_mutex_destroy(&gameState.mutex);
    pthread_mutex_destroy(&uiState.mutex);
    pthread_mutex_destroy(&eventQueueMutex);
    pthread_mutex_destroy(&gameEngineThreadExitMutex);
    pthread_cond_destroy(&gameEngineThreadExitCond);
    pthread_mutex_destroy(&ghostExitMutex);
    pthread_cond_destroy(&ghostExitCond);
    
    return 0;
}
