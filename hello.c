#include <SFML/Graphics.h>
#include <SFML/System.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h> 
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
pthread_mutex_t ghost_house_entrance_mutex;
sem_t keys;        // Semaphore for keys
sem_t exit_permits; // Semaphore for exit permits
#define TOTAL_KEYS 2        // Number of available keys
#define TOTAL_EXIT_PERMITS 2 // Number of available exit permits


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
    SCREEN_GAME_OVER  // Add this new screen type
} GameScreen;

// UI state structure
typedef struct {
    GameScreen currentScreen;
    int selectedMenuItem;
    bool needsRedraw;
    pthread_mutex_t mutex;
    char username[32];          // Add this for username storage
    bool enteringUsername;      // Add this flag
    int usernameCursorPos;      // Track cursor position
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
    int ghostType; // 1-4 for different ghost types/behaviors
} Ghost;


//Ghost variables
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
    float powerPelletDuration;  // Track duration instead of start time
    bool ghostVulnerable;
    float ghostVulnerableDuration; // Track duration instead of start time
    int pacmanStartRow;  // Pacman's original starting row
    int pacmanStartCol;  // Pacman's original starting column
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
    "=..................=",
    "=..0.....=.....0...=",
    "=...=...=====....=..",
    "=...=...=.....=..=..",
    "=.....=.........=..=",
    "=..0...........==..=",
    "=.....=####....=...=",
    "==..========....=..=",
    "=....=.......0......",
    "=.===............=..",
    "....0.........====..",
    ".....=....0...==....",
    "....====....==...0..",
    "....=...............",
    "....=....@.....=....",
    "....=..........=....",
    "...........0...=....",
    "..0.....===.........",
    ".......=====.....=..",
    ".....=========....=="
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
        // File doesn't exist yet, that's okay
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

// Function to save scores to file
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
    // Check if the score is high enough to be on the board
    if (scoreCount < MAX_SCORES || score > scoreBoard[scoreCount-1].score) {
        // Find the correct position to insert
        int insertPos = scoreCount;
        for (int i = 0; i < scoreCount; i++) {
            if (score > scoreBoard[i].score) {
                insertPos = i;
                break;
            }
        }

        // If we're at max capacity, we'll replace the lowest score
        if (scoreCount == MAX_SCORES) {
            insertPos = MAX_SCORES - 1;
        }

        // Make room for the new score if needed
        if (scoreCount < MAX_SCORES) {
            scoreCount++;
        }

        // Shift scores down
        for (int i = scoreCount-1; i > insertPos; i--) {
            scoreBoard[i] = scoreBoard[i-1];
        }

        // Insert the new score
        strncpy(scoreBoard[insertPos].username, username, 31);
        scoreBoard[insertPos].username[31] = '\0';
        scoreBoard[insertPos].score = score;

        // Save to file
        saveScores();
    }
}
void saveOriginalBoard() {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            // Copy everything except ghosts and pacman
            if (gameState.board[i][j] != '#' && gameState.board[i][j] != '@') {
                gameState.originalBoard[i][j] = gameState.board[i][j];
            } else {
                gameState.originalBoard[i][j] = ' '; // Treat ghosts and pacman as empty in original
            }
        }
    }
}

void init_ghost_house_resources() {
    pthread_mutex_init(&ghost_house_entrance_mutex, NULL);
    sem_init(&keys, 0, 2); // 2 keys available
    sem_init(&exit_permits, 0, 2); // 2 exit permits available
    
    // Define ghost house positions
    #define GHOST_HOUSE_EXIT_ROW (ROWS / 2)
    #define GHOST_HOUSE_EXIT_COL (COLS / 2)
    
    // Initialize ghost starting positions
    for (int i = 0; i < MAX_GHOSTS; i++) {
        ghosts[i].startRow = GHOST_HOUSE_EXIT_ROW - 1 + (i % 2);
        ghosts[i].startCol = GHOST_HOUSE_EXIT_COL - 1 + (i / 2);
        ghosts[i].row = ghosts[i].startRow;
        ghosts[i].col = ghosts[i].startCol;
        ghosts[i].inGhostHouse = true;
    }
}
// 3. Implement the ghost house functionality
void leave_ghost_house(Ghost *ghost) {
    // Try to acquire resources in a specific order to prevent deadlock
    sem_wait(&keys);
    printf("Ghost %d acquired a key\n", ghost->id);
    
    sem_wait(&exit_permits);
    printf("Ghost %d acquired an exit permit\n", ghost->id);
    
    // Lock the entrance while exiting
    pthread_mutex_lock(&ghost_house_entrance_mutex);
    printf("Ghost %d is leaving the ghost house\n", ghost->id);
    
    // Set the ghost's position to just outside the ghost house
    pthread_mutex_lock(&mutex);
    ghost->row = GHOST_HOUSE_EXIT_ROW;
    ghost->col = GHOST_HOUSE_EXIT_COL;
    ghost->inGhostHouse = false;
    pthread_mutex_unlock(&mutex);
    
    // Simulate time to exit
    usleep(50000);
    pthread_mutex_unlock(&ghost_house_entrance_mutex);
    
    // Release resources in reverse order
    sem_post(&exit_permits);
    sem_post(&keys);
    printf("Ghost %d has left the ghost house and released resources\n", ghost->id);
}


void return_to_ghost_house(Ghost *ghost) {
    pthread_mutex_lock(&ghost_house_entrance_mutex);
    printf("Ghost %d is returning to the ghost house\n", ghost->id);
    
    // Set the ghost's position to inside the ghost house
    pthread_mutex_lock(&mutex);
    ghost->row = ghost->startRow;
    ghost->col = ghost->startCol;
    ghost->inGhostHouse = true;
    ghost->isVulnerable = false;
    pthread_mutex_unlock(&mutex);
    
    // Simulate time to enter
    usleep(50000);
    pthread_mutex_unlock(&ghost_house_entrance_mutex);
    printf("Ghost %d has returned to the ghost house\n", ghost->id);
}


void cleanup_ghost_house_resources() {
    pthread_mutex_destroy(&ghost_house_entrance_mutex);
    sem_destroy(&keys);
    sem_destroy(&exit_permits);
}


void return_to_ghost_house(int ghost_id) {
    pthread_mutex_lock(&ghost_house_entrance_mutex);
    printf("Ghost %d is returning to the ghost house\n", ghost_id);
    // Simulate time to enter
    usleep(50000);
    pthread_mutex_unlock(&ghost_house_entrance_mutex);
    printf("Ghost %d has returned to the ghost house\n", ghost_id);
}
// Function to initialize ghosts at their starting positions
void initGhosts() {
    // Define starting positions for ghosts 
    init_ghost_house_resources();
    int ghostStartPositions[MAX_GHOSTS][2] = {
        {7, 9},   // Ghost 1 - Middle of the board
        {7, 10},  // Ghost 2 - Middle of the board
        {6, 9},   // Ghost 3 - Middle of the board
        {6, 10}   // Ghost 4 - Middle of the board
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
        ghosts[i].ghostType = i + 1;  // Each ghost has a different type/behavior
        
        // Place ghost on the board
        gameState.board[ghosts[i].row][ghosts[i].col] = '#';
    }
    
    pthread_mutex_unlock(&gameState.mutex);
}

bool isValidGhostMove(int row, int col) {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) {
        return false;
    }
    
    char cell = gameState.board[row][col];
    // Ghosts can move through empty spaces, dots, and power pellets
    return (cell != '=' && cell != '#');
}

// Calculate direction weights based on ghost type and target
DirectionWeights calculateDirectionWeights(Ghost* ghost, int targetRow, int targetCol) {
    DirectionWeights weights = {1.0f, 1.0f, 1.0f, 1.0f};  // Default equal weights
    
    // Get ghost's current position
    int currentRow = ghost->row;
    int currentCol = ghost->col;
    
    // Calculate direction to target
    int rowDiff = targetRow - currentRow;
    int colDiff = targetCol - currentCol;
    
    // Based on ghost type, adjust weights
    switch (ghost->ghostType) {
        case 1:  // Red ghost (Blinky) - Direct chase
            // Increase weight in direction of Pacman
            if (rowDiff < 0) weights.up *= 3.0f;
            if (rowDiff > 0) weights.down *= 3.0f;
            if (colDiff < 0) weights.left *= 3.0f;
            if (colDiff > 0) weights.right *= 3.0f;
            break;
            
        case 2:  // Pink ghost (Pinky) - Ambush ahead of Pacman
            // Try to get ahead of Pacman's direction
            pthread_mutex_lock(&gameState.mutex);
            Direction pacmanDir = gameState.currentDirection;
            pthread_mutex_unlock(&gameState.mutex);
            
            // Predict 4 spaces ahead
            int aheadRow = targetRow;
            int aheadCol = targetCol;
            
            switch (pacmanDir) {
                case DIR_UP:    aheadRow -= 4; break;
                case DIR_DOWN:  aheadRow += 4; break;
                case DIR_LEFT:  aheadCol -= 4; break;
                case DIR_RIGHT: aheadCol += 4; break;
                default: break;
            }
            
            // Now target that position instead
            rowDiff = aheadRow - currentRow;
            colDiff = aheadCol - currentCol;
            
            if (rowDiff < 0) weights.up *= 2.5f;
            if (rowDiff > 0) weights.down *= 2.5f;
            if (colDiff < 0) weights.left *= 2.5f;
            if (colDiff > 0) weights.right *= 2.5f;
            break;
            
        case 3:  // Blue ghost (Inky) - Somewhat erratic but tends toward Pacman
            // Mix of targeting and random movement
            if (rowDiff < 0) weights.up *= 2.0f;
            if (rowDiff > 0) weights.down *= 2.0f;
            if (colDiff < 0) weights.left *= 2.0f;
            if (colDiff > 0) weights.right *= 2.0f;
            
            // Add randomness
            weights.up *= (1.0f + ((float)rand() / RAND_MAX));
            weights.down *= (1.0f + ((float)rand() / RAND_MAX));
            weights.left *= (1.0f + ((float)rand() / RAND_MAX));
            weights.right *= (1.0f + ((float)rand() / RAND_MAX));
            break;
            
        case 4:  // Orange ghost (Clyde) - Chase when far, scatter when close
            // Calculate distance to Pacman
            float distance = sqrt((rowDiff * rowDiff) + (colDiff * colDiff));
            
            if (distance > 8.0f) {
                // Chase like Blinky when far
                if (rowDiff < 0) weights.up *= 3.0f;
                if (rowDiff > 0) weights.down *= 3.0f;
                if (colDiff < 0) weights.left *= 3.0f;
                if (colDiff > 0) weights.right *= 3.0f;
            } else {
                // Scatter when close - go to bottom-left corner
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
    
    // If ghost is vulnerable (after power pellet), reverse the weights
    if (ghost->isVulnerable) {
        float temp = weights.up;
        weights.up = weights.down;
        weights.down = temp;
        
        temp = weights.left;
        weights.left = weights.right;
        weights.right = temp;
    }
    
    // Discourage reversal (going back the way it came)
    switch (ghost->direction) {
        case DIR_UP:    weights.down *= 0.2f; break;
        case DIR_DOWN:  weights.up *= 0.2f; break;
        case DIR_LEFT:  weights.right *= 0.2f; break;
        case DIR_RIGHT: weights.left *= 0.2f; break;
        default: break;
    }
    
    return weights;
}

// 2. Improved ghost direction selection logic to make ghosts less aggressive
Direction chooseDirection(Ghost *ghost) {
    // Define possible directions
    Direction possibleDirections[4] = {UP, DOWN, LEFT, RIGHT};
    int validDirections[4];
    int numValidDirections = 0;
    
    // Check which directions are valid (no wall)
    if (ghost->row > 0 && board[ghost->row - 1][ghost->col] != '#') {
        validDirections[numValidDirections++] = UP;
    }
    if (ghost->row < ROWS - 1 && board[ghost->row + 1][ghost->col] != '#') {
        validDirections[numValidDirections++] = DOWN;
    }
    if (ghost->col > 0 && board[ghost->row][ghost->col - 1] != '#') {
        validDirections[numValidDirections++] = LEFT;
    }
    if (ghost->col < COLS - 1 && board[ghost->row][ghost->col + 1] != '#') {
        validDirections[numValidDirections++] = RIGHT;
    }
    
    // If there are no valid directions, maintain current direction
    if (numValidDirections == 0) {
        return ghost->direction;
    }
    
    // Ghost behavior depends on whether it's vulnerable
    if (ghost->isVulnerable) {
        // When vulnerable, try to move away from Pacman
        int maxDistance = -1;
        Direction bestDirection = ghost->direction;
        
        for (int i = 0; i < numValidDirections; i++) {
            int newRow = ghost->row;
            int newCol = ghost->col;
            
            switch (validDirections[i]) {
                case UP: newRow--; break;
                case DOWN: newRow++; break;
                case LEFT: newCol--; break;
                case RIGHT: newCol++; break;
            }
            
            // Calculate Manhattan distance to Pacman
            int distance = abs(newRow - pacmanRow) + abs(newCol - pacmanCol);
            
            // Prefer the direction that maximizes distance
            if (distance > maxDistance) {
                maxDistance = distance;
                bestDirection = validDirections[i];
            }
        }
        
        // 80% chance to choose the best direction, 20% chance for random movement
        if (rand() % 100 < 80) {
            return bestDirection;
        } else {
            return validDirections[rand() % numValidDirections];
        }
    } else {
        // Normal ghost behavior - 60% chance to move toward Pacman, 40% random
        if (rand() % 100 < 60) {
            int minDistance = INT_MAX;
            Direction bestDirection = ghost->direction;
            
            for (int i = 0; i < numValidDirections; i++) {
                int newRow = ghost->row;
                int newCol = ghost->col;
                
                switch (validDirections[i]) {
                    case UP: newRow--; break;
                    case DOWN: newRow++; break;
                    case LEFT: newCol--; break;
                    case RIGHT: newCol++; break;
                }
                
                // Calculate Manhattan distance to Pacman
                int distance = abs(newRow - pacmanRow) + abs(newCol - pacmanCol);
                
                // Prefer the direction that minimizes distance
                if (distance < minDistance) {
                    minDistance = distance;
                    bestDirection = validDirections[i];
                }
            }
            
            return bestDirection;
        } else {
            // Random movement
            return validDirections[rand() % numValidDirections];
        }
    }
}


// 8. Timer function for ghost vulnerability
void* vulnerabilityTimer(void* arg) {
    // Vulnerable for 10 seconds
    usleep(10000000);
    
    pthread_mutex_lock(&mutex);
    // End vulnerability for all ghosts
    for (int i = 0; i < NUM_GHOSTS; i++) {
        if (!ghosts[i].needsRespawn) { // Only if not already being respawned
            ghosts[i].isVulnerable = false;
        }
    }
    pthread_mutex_unlock(&mutex);
    
    return NULL;
}
// 7. Improved power pellet handling
void eatPowerPellet() {
    pthread_mutex_lock(&mutex);
    
    // Make ghosts vulnerable
    for (int i = 0; i < NUM_GHOSTS; i++) {
        ghosts[i].isVulnerable = true;
    }
    
    // Update score
    pthread_mutex_lock(&score_mutex);
    score += 50;
    pthread_mutex_unlock(&score_mutex);
    
    pthread_mutex_unlock(&mutex);
    
    // Start a timer to end vulnerability
    pthread_t timerThread;
    pthread_create(&timerThread, NULL, vulnerabilityTimer, NULL);
}

// 1. Modify the moveGhost function to make ghost movement more predictable 
// and ensure they can be eaten when vulnerable
void moveGhost(Ghost *ghost) {
    pthread_mutex_lock(&mutex);
    
    // Get current position
    int currentRow = ghost->row;
    int currentCol = ghost->col;
    
    // Clear current position on board
    board[currentRow][currentCol] = ghost->cellContent;
    
    // Choose a new direction with improved logic
    Direction newDirection = chooseDirection(ghost);
    
    // Calculate new position
    int newRow = currentRow;
    int newCol = currentCol;
    
    // Move based on direction - with collision detection
    switch (newDirection) {
        case UP:
            if (currentRow > 0 && board[currentRow - 1][currentCol] != '#') {
                newRow = currentRow - 1;
            }
            break;
        case DOWN:
            if (currentRow < ROWS - 1 && board[currentRow + 1][currentCol] != '#') {
                newRow = currentRow + 1;
            }
            break;
        case LEFT:
            if (currentCol > 0 && board[currentRow][currentCol - 1] != '#') {
                newCol = currentCol - 1;
            }
            break;
        case RIGHT:
            if (currentCol < COLS - 1 && board[currentRow][currentCol + 1] != '#') {
                newCol = currentCol + 1;
            }
            break;
    }
    
    // Check if Pacman is at the new position
    if (newRow == pacmanRow && newCol == pacmanCol) {
        if (ghost->isVulnerable) {
            // Ghost is eaten by Pacman
            pthread_mutex_lock(&score_mutex);
            score += 200;
            pthread_mutex_unlock(&score_mutex);
            
            // Ghost returns to starting position
            ghost->needsRespawn = true;
            ghost->cellContent = ' ';  // Clear the cell content
            
            // Move ghost back to ghost house
            ghost->row = ghost->startRow;
            ghost->col = ghost->startCol;
            ghost->isVulnerable = false;
            
            // Set the ghost house flag to indicate ghost is in the house
            ghost->inGhostHouse = true;
        } else if (!invincible) {
            // Pacman loses a life
            pthread_mutex_lock(&life_mutex);
            lives--;
            pthread_mutex_unlock(&life_mutex);
            
            // Reset Pacman position
            pacmanRow = PACMAN_START_ROW;
            pacmanCol = PACMAN_START_COL;
            
            // Reset all ghosts
            for (int i = 0; i < NUM_GHOSTS; i++) {
                ghosts[i].row = ghosts[i].startRow;
                ghosts[i].col = ghosts[i].startCol;
                ghosts[i].isVulnerable = false;
                ghosts[i].inGhostHouse = true;
            }
            
            // Quick pause to make death noticeable
            pthread_mutex_unlock(&mutex);
            usleep(500000);
            pthread_mutex_lock(&mutex);
        }
    } else {
        // No collision with Pacman, update ghost position
        ghost->cellContent = board[newRow][newCol];
        ghost->row = newRow;
        ghost->col = newCol;
        
        // Mark the new position on the board
        if (ghost->isVulnerable) {
            board[newRow][newCol] = 'v'; // Vulnerable ghost
        } else {
            board[newRow][newCol] = 'G'; // Regular ghost
        }
    }
    
    pthread_mutex_unlock(&mutex);
}

// Forward declarations
typedef struct TimerThreadArgs TimerThreadArgs;
void* ghostTimerThread(void* arg);

// Timer thread arguments structure
typedef struct TimerThreadArgs {
    sem_t* semaphore;
    int intervalMs;
    bool isRunning;
} TimerThreadArgs;

bool game_over = false;



void* pacman(void* arg) {
    // Set up keyboard input
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    fd_set rfds;
    struct timeval tv;
    int retval;

    // Pacman movement direction
    int direction = 0;  // 0: up, 1: down, 2: left, 3: right

    while (!game_over) {
        // Check for keyboard input
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        retval = select(1, &rfds, NULL, NULL, &tv);

        if (retval) {
            char c;
            read(0, &c, 1);
            switch (c) {
                case 'w':
                    direction = 0;  // up
                    break;
                case 's':
                    direction = 1;  // down
                    break;
                case 'a':
                    direction = 2;  // left
                    break;
                case 'd':
                    direction = 3;  // right
                    break;
                case 'q':
                    game_over = 1;
                    break;
            }
        }

        // Move Pacman based on direction
        pthread_mutex_lock(&mutex);
        int new_row = pacman_row;
        int new_col = pacman_col;

        switch (direction) {
            case 0:  // up
                new_row--;
                break;
            case 1:  // down
                new_row++;
                break;
            case 2:  // left
                new_col--;
                break;
            case 3:  // right
                new_col++;
                break;
        }

        // Check if the new position is valid and update Pacman's position
        if (new_row >= 0 && new_row < ROWS && new_col >= 0 && new_col < COLS &&
            board[new_row][new_col] != '#') {
            // Check if Pacman eats a dot
            if (board[new_row][new_col] == '.') {
                pthread_mutex_lock(&score_mutex);
                score += 10;
                dots_remaining--;
                pthread_mutex_unlock(&score_mutex);
            }
            // Check if Pacman eats a power pellet
            else if (board[new_row][new_col] == 'O') {
                pthread_mutex_lock(&score_mutex);
                score += 50;
                pthread_mutex_unlock(&score_mutex);
                power_pellet_active = 1;
                // Create a thread to handle the power pellet timer
                pthread_t timer_thread;
                pthread_create(&timer_thread, NULL, power_pellet_timer, NULL);
            }

            // Update Pacman's position on the board
            board[pacman_row][pacman_col] = ' ';
            pacman_row = new_row;
            pacman_col = new_col;
            board[pacman_row][pacman_col] = 'P';

            // Check if Pacman collides with a ghost
            for (int i = 0; i < NUM_GHOSTS; i++) {
                if (ghost_row[i] == pacman_row && ghost_col[i] == pacman_col) {
                    if (power_pellet_active) {
                        // Pacman eats the ghost
                        pthread_mutex_lock(&score_mutex);
                        score += 200;
                        pthread_mutex_unlock(&score_mutex);
                        // Respawn ghost at starting position
                        ghost_row[i] = GHOST_START_ROW;
                        ghost_col[i] = GHOST_START_COL;
                    } else {
                        // Ghost eats Pacman
                        pthread_mutex_lock(&life_mutex);
                        lives--;
                        pthread_mutex_unlock(&life_mutex);
                        // Respawn Pacman at starting position
                        pacman_row = PACMAN_START_ROW;
                        pacman_col = PACMAN_START_COL;
                        // Respawn all ghosts at starting positions
                        for (int j = 0; j < NUM_GHOSTS; j++) {
                            ghost_row[j] = GHOST_START_ROW;
                            ghost_col[j] = GHOST_START_COL;
                        }
                        break;
                    }
                }
            }
        }

        // Check if all dots are eaten
        pthread_mutex_lock(&score_mutex);
        if (dots_remaining == 0) {
            game_over = 1;
            win = 1;
        }
        pthread_mutex_unlock(&score_mutex);

        // Check if all lives are lost
        pthread_mutex_lock(&life_mutex);
        if (lives == 0) {
            game_over = 1;
            win = 0;
        }
        pthread_mutex_unlock(&life_mutex);

        pthread_mutex_unlock(&mutex);
        usleep(200000);  // Sleep for 200ms
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);

    return NULL;
}
// Main function for ghost thread
void* ghostThreadFunc(void* arg) {
    Ghost* ghost = (Ghost*)arg;  // Changed from int to Ghost* since errors suggest ghost is a struct
    int ghost_id = ghost->id;    // Access id through the struct
    
    while (!game_over) {
        // Initial state - ghost in the house
        leave_ghost_house(ghost_id);
        
        // Different speeds for different ghosts
        int moveInterval = 200 + (ghost->id * 50);  // fixed: Ghost->id to ghost->id
        
        // Use a semaphore to control movement timing for this ghost
        sem_t moveSemaphore;
        sem_init(&moveSemaphore, 0, 0);
        
        // Need a separate thread to signal the semaphore at the appropriate intervals
        pthread_t timerThread;
        TimerThreadArgs timerArgs;
        timerArgs.semaphore = &moveSemaphore;
        timerArgs.intervalMs = moveInterval;
        timerArgs.isRunning = true;
        
        // Start the timer thread
        pthread_create(&timerThread, NULL, ghostTimerThread, &timerArgs);
        
        while (ghostThreadsRunning) {
            // Wait on semaphore - this is not an explicit wait mechanism but a synchronization primitive
            sem_wait(&moveSemaphore);
            
            // Check if the game is running and not paused
            pthread_mutex_lock(&gameState.mutex);
            bool gameRunning = gameState.gameRunning;
            bool gamePaused = gameState.gamePaused;
            pthread_mutex_unlock(&gameState.mutex);
            
            if (!gameRunning) {
                game_over = true;
                break;
            }
            
            pthread_mutex_lock(&uiState.mutex);
            bool isPlayScreen = (uiState.currentScreen == SCREEN_PLAY);
            pthread_mutex_unlock(&uiState.mutex);
            
            if (isPlayScreen && !gamePaused) {
                // Check if ghost needs to respawn
                if (ghost->needsRespawn) {  // fixed: ghost->needsRespawn
                    pthread_mutex_lock(&gameState.mutex);
                    ghost->row = ghost->respawnRow;
                    ghost->col = ghost->respawnCol;
                    ghost->isVulnerable = false;
                    ghost->needsRespawn = false;
                    gameState.board[ghost->row][ghost->col] = '#';
                    pthread_mutex_unlock(&gameState.mutex);
                }
                
                // Update ghost vulnerability status
                pthread_mutex_lock(&gameState.mutex);
                ghost->isVulnerable = gameState.ghostVulnerable;
                pthread_mutex_unlock(&gameState.mutex);
                
                // Get Pacman's current position
                int pacmanRow, pacmanCol;
                pthread_mutex_lock(&gameState.mutex);
                pacmanRow = gameState.pacmanRow;
                pacmanCol = gameState.pacmanCol;
                pthread_mutex_unlock(&gameState.mutex);
                
                // Calculate direction weights
                DirectionWeights weights = calculateDirectionWeights(ghost, pacmanRow, pacmanCol);
                
                // Choose direction
                Direction newDirection = chooseGhostDirection(ghost, weights);
                
                // Move ghost
                moveGhost(ghost, newDirection);
            }
        }
        
        // Stop the timer thread
        timerArgs.isRunning = false;
        pthread_join(timerThread, NULL);
        
        // Clean up
        sem_destroy(&moveSemaphore);
        
        // Signal that this ghost thread has exited
        pthread_mutex_lock(&ghostExitMutex);
        ghostThreadsExited++;
        
        // If this is the last ghost thread to exit, signal the main thread
        if (ghostThreadsExited >= MAX_GHOSTS) {
            pthread_cond_signal(&ghostExitCond);
        }
        pthread_mutex_unlock(&ghostExitMutex);
        
        // When ghost is eaten or needs to return
        return_to_ghost_house(ghost_id);
        
        // Wait for a bit before trying to leave again
        usleep(random() % 1000000);
    }
    
    return NULL;
}
// Timer thread function - this will control the timing for ghost movement
void* ghostTimerThread(void* arg) {
    TimerThreadArgs* args = (TimerThreadArgs*)arg;
    
    // Use pthread_cond_timedwait for timing (a proper synchronization primitive)
    pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t timerCond = PTHREAD_COND_INITIALIZER;
    
    struct timespec ts;
    
    while (args->isRunning) {
        // Get current time
        clock_gettime(CLOCK_REALTIME, &ts);
        
        // Add interval
        ts.tv_nsec += (args->intervalMs % 1000) * 1000000;
        ts.tv_sec += args->intervalMs / 1000 + (ts.tv_nsec / 1000000000);
        ts.tv_nsec %= 1000000000;
        
        // Use condition variable with timeout (synchronization primitive)
        pthread_mutex_lock(&timerMutex);
        pthread_cond_timedwait(&timerCond, &timerMutex, &ts);  
        pthread_mutex_unlock(&timerMutex);
        
        // Signal the ghost thread to move if still running
        if (args->isRunning) {
            sem_post(args->semaphore);
        }
    }
    
    pthread_mutex_destroy(&timerMutex);
    pthread_cond_destroy(&timerCond);
    
    return NULL;
}

void startGhostThreads() {
    // Reset the exit counter when starting threads
    pthread_mutex_lock(&ghostExitMutex);
    ghostThreadsExited = 0;
    pthread_mutex_unlock(&ghostExitMutex);
    
    for (int i = 0; i < MAX_GHOSTS; i++) {
        if (pthread_create(&ghosts[i].thread, NULL, ghostThreadFunc, &ghosts[i]) != 0) {
            printf("Error creating ghost thread %d\n", i);
        }
    }
}

// Function to stop and clean up ghost threads without using pthread_join
void stopGhostThreads() {
    // Signal all ghost threads to stop
    ghostThreadsRunning = false;
    
    // Wait for all ghost threads to exit
    pthread_mutex_lock(&ghostExitMutex);
    while (ghostThreadsExited < MAX_GHOSTS) {
        // Wait on condition variable until all threads have exited
        pthread_cond_wait(&ghostExitCond, &ghostExitMutex);
    }
    pthread_mutex_unlock(&ghostExitMutex);
}

// Add this to your cleanup code (in main, before returning)
void cleanupGhostThreadSync() {
    pthread_mutex_destroy(&ghostExitMutex);
    pthread_cond_destroy(&ghostExitCond);
}

void renderGameOver(sfRenderWindow* window, sfFont* font) {
    // First time showing game over screen? Add the score
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
    
    // Display final score
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
    strcpy(uiState.username, "Unknown");  // Default username
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

// 9. Improved pacman movement for smoother gameplay
void movePacman() {
    pthread_mutex_lock(&mutex);
    
    // Get current position
    int currentRow = pacmanRow;
    int currentCol = pacmanCol;
    
    // Clear current position
    board[currentRow][currentCol] = ' ';
    
    // Calculate new position based on current direction
    int newRow = currentRow;
    int newCol = currentCol;
    
    switch (pacmanDirection) {
        case UP:
            if (currentRow > 0 && board[currentRow - 1][currentCol] != '#') {
                newRow = currentRow - 1;
            }
            break;
        case DOWN:
            if (currentRow < ROWS - 1 && board[currentRow + 1][currentCol] != '#') {
                newRow = currentRow + 1;
            }
            break;
        case LEFT:
            if (currentCol > 0 && board[currentRow][currentCol - 1] != '#') {
                newCol = currentCol - 1;
            }
            break;
        case RIGHT:
            if (currentCol < COLS - 1 && board[currentRow][currentCol + 1] != '#') {
                newCol = currentCol + 1;
            }
            break;
    }
    
    // Check what's at the new position
    char cell = board[newRow][newCol];
    
    // Handle different cell types
    switch (cell) {
        case '.': // Regular dot
            pthread_mutex_lock(&score_mutex);
            score += 10;
            dotsRemaining--;
            pthread_mutex_unlock(&score_mutex);
            break;
            
        case 'O': // Power pellet
            eatPowerPellet();
            break;
            
        case 'G': // Regular ghost
            if (!invincible) {
                pthread_mutex_lock(&life_mutex);
                lives--;
                pthread_mutex_unlock(&life_mutex);
                
                // Reset positions
                pacmanRow = PACMAN_START_ROW;
                pacmanCol = PACMAN_START_COL;
                
                for (int i = 0; i < NUM_GHOSTS; i++) {
                    ghosts[i].row = ghosts[i].startRow;
                    ghosts[i].col = ghosts[i].startCol;
                    ghosts[i].isVulnerable = false;
                    ghosts[i].inGhostHouse = true;
                }
                
                pthread_mutex_unlock(&mutex);
                usleep(500000); // Short pause
                return;
            }
            break;
            
        case 'v': // Vulnerable ghost
            // Find which ghost is at this position
            for (int i = 0; i < NUM_GHOSTS; i++) {
                if (ghosts[i].row == newRow && ghosts[i].col == newCol) {
                    // Eat the ghost
                    pthread_mutex_lock(&score_mutex);
                    score += 200;
                    pthread_mutex_unlock(&score_mutex);
                    
                    // Send ghost back to ghost house
                    ghosts[i].needsRespawn = true;
                    ghosts[i].inGhostHouse = true;
                    ghosts[i].row = ghosts[i].startRow;
                    ghosts[i].col = ghosts[i].startCol;
                    break;
                }
            }
            break;
    }
    
    // Update Pacman's position
    pacmanRow = newRow;
    pacmanCol = newCol;
    board[pacmanRow][pacmanCol] = 'P';
    
    pthread_mutex_unlock(&mutex);
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

    // Render username input
    pthread_mutex_lock(&uiState.mutex);
    char usernameLabel[64];
    sprintf(usernameLabel, "Player: %s", uiState.username);
    bool enteringUsername = uiState.enteringUsername;
    int selectedItem = uiState.selectedMenuItem;  // Declare selectedItem here
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

    // Menu items rendering
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
    
    // Declare instructionsText here
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
    
    // Clean up
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
    
    // Display scores
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
                    // Find which ghost is at this position
                    for (int g = 0; g < MAX_GHOSTS; g++) {
                        if (ghosts[g].row == i && ghosts[g].col == j) {
                            // Set appropriate ghost sprite based on ghost type and vulnerability
                            sfSprite* currentGhostSprite;
                            
                            if (ghosts[g].isVulnerable) {
                                // Use vulnerable ghost texture
                                currentGhostSprite = ghost5Sprite;
                            } else {
                                // Select ghost sprite based on ghost type
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
                            break; // Found the ghost at this position
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
                    // Handle username input
                    if (event.key.code == sfKeyEnter || event.key.code == sfKeyEscape) {
                        // Finish entering username
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.enteringUsername = false;
                        uiState.needsRedraw = true;
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                    else if (event.key.code == sfKeyBackspace) {
                        // Handle backspace
                        pthread_mutex_lock(&uiState.mutex);
                        if (uiState.usernameCursorPos > 0) {
                            uiState.username[--uiState.usernameCursorPos] = '\0';
                            uiState.needsRedraw = true;
                        }
                        pthread_mutex_unlock(&uiState.mutex);
                    }
                }
                else {
                    // Handle regular menu navigation
                    if (event.key.code == sfKeyU) {
                        // Start entering username
                        pthread_mutex_lock(&uiState.mutex);
                        uiState.enteringUsername = true;
                        uiState.username[0] = '\0'; // Clear current username
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
                    // Backspace (handled in KeyPressed)
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

// Timer thread function for game ticks
void* gameTickTimerThread(void* arg) {
    TimerThreadArgs* args = (TimerThreadArgs*)arg;
    
    pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t timerCond = PTHREAD_COND_INITIALIZER;
    
    struct timespec ts;
    
    while (args->isRunning) {
        // Get current time
        clock_gettime(CLOCK_REALTIME, &ts);
        
        // Add interval
        ts.tv_nsec += (args->intervalMs % 1000) * 1000000;
        ts.tv_sec += args->intervalMs / 1000 + (ts.tv_nsec / 1000000000);
        ts.tv_nsec %= 1000000000;
        
        // Use condition variable with timeout
        pthread_mutex_lock(&timerMutex);
        pthread_cond_timedwait(&timerCond, &timerMutex, &ts);  
        pthread_mutex_unlock(&timerMutex);
        
        // Signal the game engine thread to process a tick
        if (args->isRunning) {
            sem_post(args->semaphore);
        }
    }
    
    pthread_mutex_destroy(&timerMutex);
    pthread_cond_destroy(&timerCond);
    
    return NULL;
}

void* gameEngineThreadFunc(void* arg) {
    // Create semaphores for synchronization
    sem_t gameTick;
    sem_init(&gameTick, 0, 0);
    
    // Create frame synchronization primitives
    pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t frameCond = PTHREAD_COND_INITIALIZER;
    
    // Create a separate thread for timing that signals when to update the game
    pthread_t tickThread;
    TimerThreadArgs tickArgs;
    tickArgs.semaphore = &gameTick;
    tickArgs.intervalMs = 200; // moveInterval in milliseconds
    tickArgs.isRunning = true;
    
    pthread_create(&tickThread, NULL, gameTickTimerThread, &tickArgs);
    
    while (true) {
        // Wait for a tick signal from the timer thread
        sem_wait(&gameTick);
        
        pthread_mutex_lock(&gameState.mutex);
        bool gameRunning = gameState.gameRunning;
        bool gamePaused = gameState.gamePaused;
        float deltaTime = 0.2f; // Fixed time step
        pthread_mutex_unlock(&gameState.mutex);
        
        if (!gameRunning) {
            break;
        }
        
        // Process input events
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
            // Move Pac-Man on every tick
            movePacman();
            
            // Signal that a game step has completed (for potential rendering thread)
            pthread_mutex_lock(&frameMutex);
            pthread_cond_broadcast(&frameCond);
            pthread_mutex_unlock(&frameMutex);
            
            pthread_mutex_lock(&gameState.mutex);
            
            // Update power pellet state
            if (gameState.powerPelletActive) {
                gameState.powerPelletDuration += deltaTime;
                if (gameState.powerPelletDuration >= 10.0f) {
                    gameState.powerPelletActive = false;
                    gameState.ghostVulnerable = false;
                }
            }
            
            // Update ghost vulnerability state
            if (gameState.ghostVulnerable) {
                gameState.ghostVulnerableDuration += deltaTime;
                
                if (gameState.ghostVulnerableDuration >= 10.0f) {
                    gameState.ghostVulnerable = false;
                }
            }
            
            pthread_mutex_unlock(&gameState.mutex);
        }
    }
    
    // Stop the timer thread
    tickArgs.isRunning = false;
    pthread_join(tickThread, NULL);
    
    // Clean up resources
    sem_destroy(&gameTick);
    pthread_mutex_destroy(&frameMutex);
    pthread_cond_destroy(&frameCond);
    
    // Signal that the thread has exited
    pthread_mutex_lock(&gameEngineThreadExitMutex);
    gameEngineThreadExited = true;
    pthread_cond_signal(&gameEngineThreadExitCond);
    pthread_mutex_unlock(&gameEngineThreadExitMutex);
    
    return NULL;
}

int main() {
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
    
    // Load textures
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
    // Set initial game state with score added flag as false
    static bool scoreAdded = false;
    scoreAdded = false;
    initGameState();
    
    // Initialize clocks
    gameClock = sfClock_create();
    pelletBlinkClock = sfClock_create();
    
    // Start game engine thread
    pthread_t gameEngineThread;
    if (pthread_create(&gameEngineThread, NULL, gameEngineThreadFunc, NULL) != 0) {
        printf("Error creating game engine thread\n");
        return -1;
    }
    
    // Start ghost threads
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
        
        // Check for screen transitions
        InputEvent event;
        while (getNextInputEvent(&event)) {
            if (event.eventType == EVENT_SCREEN_CHANGE && event.data == SCREEN_PLAY) {
                // Reset the score added flag when starting a new game
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
    
    // Signal threads to exit
    pthread_mutex_lock(&gameState.mutex);
    gameState.gameRunning = false;
    pthread_mutex_unlock(&gameState.mutex);
    
    // Wait for game engine thread to exit using condition variable
    pthread_mutex_lock(&gameEngineThreadExitMutex);
    while (!gameEngineThreadExited) {
        pthread_cond_wait(&gameEngineThreadExitCond, &gameEngineThreadExitMutex);
    }
    pthread_mutex_unlock(&gameEngineThreadExitMutex);
    
    // Wait for ghost threads to exit
    pthread_mutex_lock(&ghostExitMutex);
    ghostThreadsRunning = false;
    while (ghostThreadsExited < MAX_GHOSTS) {
        pthread_cond_wait(&ghostExitCond, &ghostExitMutex);
    }
    pthread_mutex_unlock(&ghostExitMutex);
    
    // Clean up clocks
    sfClock_destroy(gameClock);
    sfClock_destroy(pelletBlinkClock);
    
    // Clean up shapes
    sfRectangleShape_destroy(wall);
    sfCircleShape_destroy(dot);
    sfCircleShape_destroy(powerPellet);
    
    // Clean up sprites
    sfSprite_destroy(pacmanSprite);
    sfSprite_destroy(ghost1Sprite);
    sfSprite_destroy(ghost2Sprite);
    sfSprite_destroy(ghost3Sprite);
    sfSprite_destroy(ghost4Sprite);
    sfSprite_destroy(ghost5Sprite);
    sfSprite_destroy(lifeSprite);
    
    // Clean up textures
    sfTexture_destroy(pacmanTexture);
    sfTexture_destroy(ghost1Texture);
    sfTexture_destroy(ghost2Texture);
    sfTexture_destroy(ghost3Texture);
    sfTexture_destroy(ghost4Texture);
    sfTexture_destroy(ghost5Texture);
    sfTexture_destroy(lifeTexture);
    
    // Clean up text
    sfText_destroy(scoreText);
    sfText_destroy(livesText);
    
    // Clean up font and window
    sfFont_destroy(font);
    sfRenderWindow_destroy(window);
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&gameState.mutex);
    pthread_mutex_destroy(&uiState.mutex);
    pthread_mutex_destroy(&eventQueueMutex);
    pthread_mutex_destroy(&gameEngineThreadExitMutex);
    pthread_cond_destroy(&gameEngineThreadExitCond);
    pthread_mutex_destroy(&ghostExitMutex);
    pthread_cond_destroy(&ghostExitCond);
    cleanup_ghost_house_resources();
    return 0;
}
