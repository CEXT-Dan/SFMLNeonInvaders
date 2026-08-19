#pragma once

// -----------------------------------------------------------------------------
//  NEON INVADERS  --  SFML 3.1.0 / C++17 (MSVC 64-bit)
//  Ported from neon_invaders.html
// -----------------------------------------------------------------------------


namespace game
{
    // =========================================================================
    //  Constants
    // =========================================================================
    constexpr float kW = 900.f;
    constexpr float kH = 640.f;
    constexpr float kTAU = 6.2831853f;

    // Player
    constexpr float kPlayerSpd = 360.f;
    constexpr float kFireCD = 0.38f;
    constexpr float kRapidCD = 0.13f;
    constexpr float kBulletVy = -540.f;

    // Enemy grid
    constexpr int   kCols = 10;
    constexpr int   kRows = 5;
    constexpr float kSpx = 56.f;
    constexpr float kSpy = 42.f;
    constexpr float kPix = 3.f;   // pixel scale for sprites

    // =========================================================================
    //  Power-up definitions
    // =========================================================================
    struct PuDef { sf::Color color; char label; const char* name; };
    inline const PuDef& puDef(char k)
    {
        static const std::map<char, PuDef> defs = {
            {'R', {sf::Color(53, 224, 255),  'R', "RAPID FIRE"}},
            {'S', {sf::Color(73, 255, 155),  'S', "SPREAD SHOT"}},
            {'H', {sf::Color(143, 143, 255), 'H', "SHIELD"}},
            {'B', {sf::Color(255, 210, 62),  '+', "+1000 PTS"}},
        };
        return defs.at(k);
    }

    // =========================================================================
    //  Entity structs
    // =========================================================================
    struct Star { float x, y, z, tw; };
    struct Enemy { const char* type; int col; float bx, by, w, h; int hp; bool alive; };
    struct Bullet { float x, y, vx, vy, w, h; };
    struct Bomb { float x, y, w, h, vy; };
    struct Particle { float x, y, vx, vy, life, t, size; sf::Color color; };
    struct PowerUp { float x, y, vy; char k; float t; };
    struct FloatText { float x, y; std::string txt; sf::Color color; float life, t; };
    struct Bunker { float x, y, w, h; int cw; std::vector<std::vector<bool>> cells{}; };
    struct Player { float x, y, w, h; float cd, rapid, spread, shield, invuln; bool alive; };
    struct Ufo { float x, y, w, h, vx; };
    struct Banner { std::string text, sub; float t; };

    enum class State { Menu, Playing, Paused, Over };

    // =========================================================================
    //  Game
    // =========================================================================
    class Game
    {
    public:
        explicit Game(sf::RenderWindow& window);
        int run();

    private:
        // Setup
        void initSprites();
        void initSounds();
        void makeWave();
        void startGame();
        void saveHi();
        void loadHi();

        // Update
        void update(float dt);
        void updateFx(float dt);
        void moveBullets(float dt);

        // Combat
        void fire();
        void killEnemy(Enemy& e);
        void dropPow(float x, float y);
        void applyPow(char k);
        void playerHit();
        void endInvasion();
        bool hitBunker(float px, float py);
        void blastBunker(Bunker& b, int c, int r);

        // Render
        void render();
        void drawBackground();
        void drawBunkers();
        void drawEnemies();
        void drawPlayer();
        void drawUfo();
        void drawBullets();
        void drawBombs();
        void drawPows();
        void drawParticles();
        void drawFloats();
        void drawHud();
        void drawOverlays();

        // Text helper
        void drawText(const std::string& str, float x, float y, float size,
            sf::Color color, const char* align, float glow);

        // Helpers
        void addFloat(float x, float y, const std::string& txt, sf::Color color);
        void burst(float x, float y, sf::Color color, int n);
        void setBanner(const std::string& text, const std::string& sub, float t);
        float randf(float a, float b);
        int   randi(int a, int b);
        void  playSnd(const std::string& name);
        bool  overlap(const Bullet& a, const Bullet& b);
        bool  overlap(const sf::FloatRect& a, const sf::FloatRect& b);

        // Members
        sf::RenderWindow& m_window;
        sf::Clock  m_clock;
        std::mt19937 m_rng;

        // State
        State m_state = State::Menu;
        float m_gt = 0.f;
        int   m_score = 0;
        int   m_hi = 0;
        int   m_lives = 3;
        int   m_wave = 1;
        float m_shake = 0.f;
        std::optional<Banner> m_banner;
        float m_waveDelay = 0.f;
        bool  m_newHi = false;
        bool  m_muted = false;

        // Entities
        std::vector<Star>      m_stars;
        std::vector<Enemy>     m_enemies;
        std::vector<Bullet>    m_bullets;
        std::vector<Bomb>      m_bombs;
        std::vector<Particle>  m_parts;
        std::vector<PowerUp>   m_pows;
        std::vector<FloatText> m_floats;
        std::vector<Bunker>    m_bunkers;

        // Grid
        float m_gridOx = 0.f, m_gridOy = 0.f;
        int   m_gridDir = 1;
        float m_noteT = 0.f;
        int   m_note = 0;

        // UFO
        std::optional<Ufo> m_ufo;
        float m_ufoTimer = 14.f;
        float m_ufoSnd = 0.f;
        float m_bombTimer = 1.5f;
        int   m_droppedThisWave = 0;

        // Player
        Player m_player;

        // Resources
        sf::Font m_font;
        std::map<std::string, sf::Texture> m_sprites;
        std::map<std::string, sf::SoundBuffer> m_sounds;
        sf::Sound m_snd;

        // Background texture (pre-rendered gradient)
        sf::Texture m_bgTex;
        sf::Sprite  m_bgSprite;
    };

} // namespace game
