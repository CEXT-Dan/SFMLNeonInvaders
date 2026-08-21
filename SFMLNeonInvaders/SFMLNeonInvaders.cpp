#include "pch.h"
#include "SFMLNeonInvaders.h"
#include "resource.h"

// =========================================================================
//  Small helpers (file-local)
// =========================================================================
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static std::string padInt(int val, int width)
{
    return std::format("{:0{}}", val, width);
}

// =========================================================================
//  Audio generation (procedural sounds)
// =========================================================================
static sf::SoundBuffer genTone(float freq, float dur, int wave, float vol, float slide)
{
    constexpr unsigned int sr = 44100;
    const std::size_t n = (std::size_t)(sr * dur);

    // SFML 3 uses std::int16_t natively
    std::vector<std::int16_t> samples(n, 0);

    const float f0 = freq;
    const float f1 = freq + slide;

    for (std::size_t i = 0; i < n; ++i)
    {
        float progress = (float)i / (float)n;
        float t = (float)i / (float)sr;

        // Exact mathematical phase integration to prevent frequency warping
        float phase = game::kTAU * (f0 * t + (f1 - f0) * progress * t * 0.5f);

        float v = 0.f;
        float p = fmodf(phase, game::kTAU);

        if (p < 0.f)
            p += game::kTAU;

        switch (wave)
        {
            case 0: v = (p < game::kTAU * 0.5f) ? 1.f : -1.f; break;              // square
            case 1: v = sinf(phase); break;                                       // sine
            case 2: v = (2.f * p / game::kTAU) - 1.f; break;                      // sawtooth
            case 3: v = (2.f / game::kTAU) * asinf(sinf(phase)); break;           // triangle
        }

        // Exponential decay envelope
        float env = vol * powf(0.001f / (vol + 0.0001f), progress);
        samples[i] = (std::int16_t)(v * env * 32000.f);
    }

    sf::SoundBuffer buf;
    std::vector<sf::SoundChannel> channelMap = { sf::SoundChannel::Mono };
    [[maybe_unused]] bool unused = buf.loadFromSamples(samples.data(), samples.size(), 1, sr, channelMap);
    return buf;
}

static sf::SoundBuffer genNoise(float dur, float vol, float cutoff)
{
    constexpr unsigned int sr = 44100;
    const std::size_t n = (std::size_t)(sr * dur);
    std::vector<std::int16_t> samples(n, 0);
    std::vector<float> raw(n);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    for (std::size_t i = 0; i < n; ++i)
        raw[i] = dist(rng) * (1.f - (float)i / (float)n);

    // One-pole low-pass
    float alpha = std::min(1.f, 2.f * 3.14159265f * cutoff / (float)sr);
    float prev = 0.f;
    for (std::size_t i = 0; i < n; ++i)
    {
        prev += alpha * (raw[i] - prev);
        samples[i] = (std::int16_t)(prev * vol * 32000.f);
    }

    sf::SoundBuffer buf;
    std::vector<sf::SoundChannel> channelMap = { sf::SoundChannel::Mono };
    [[maybe_unused]] bool unused = buf.loadFromSamples(samples.data(), samples.size(), 1, sr, channelMap);
    return buf;
}

static sf::SoundBuffer genArpeggio(const float* freqs, int count, float noteDur, float vol)
{
    constexpr unsigned int sr = 44100;
    const std::size_t noteLen = (std::size_t)(sr * noteDur);
    const std::size_t total = noteLen * (std::size_t)count;
    std::vector<std::int16_t> samples(total, 0);

    for (int fi = 0; fi < count; ++fi)
    {
        for (std::size_t i = 0; i < noteLen; ++i)
        {
            float progress = (float)i / (float)noteLen;
            float phase = game::kTAU * freqs[fi] * (float)i / (float)sr;
            float v = (sinf(phase) >= 0.f) ? 1.f : -1.f; // square
            float env = vol * powf(0.001f / (vol + 0.0001f), progress);
            samples[fi * noteLen + i] = (std::int16_t)(v * env * 32000.f);
        }
    }

    sf::SoundBuffer buf;
    std::vector<sf::SoundChannel> channelMap = { sf::SoundChannel::Mono };
    [[maybe_unused]] bool unused = buf.loadFromSamples(samples.data(), samples.size(), 1, sr, channelMap);
    return buf;
}

// Simple manual clamp replacement if clampf is an external dependency
static inline float clampFloat(float v, float min, float max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static sf::SoundBuffer mixBuffers(const sf::SoundBuffer& a, const sf::SoundBuffer& b)
{
    const std::int16_t* sa = a.getSamples();
    const std::int16_t* sb = b.getSamples();

    std::uint64_t lenA = a.getSampleCount();
    std::uint64_t lenB = b.getSampleCount();
    std::size_t maxLen = std::max(static_cast<std::size_t>(lenA), static_cast<std::size_t>(lenB));

    std::vector<std::int16_t> mixed(maxLen, 0);

    for (std::size_t i = 0; i < maxLen; ++i)
    {
        std::int16_t x = (i < lenA) ? sa[i] : 0;
        std::int16_t y = (i < lenB) ? sb[i] : 0;

        mixed[i] = static_cast<std::int16_t>(clampFloat(static_cast<float>(x + y), -32767.f, 32767.f));
    }
    std::vector<sf::SoundChannel> channelMap = { sf::SoundChannel::Mono };
    return sf::SoundBuffer(mixed.data(), mixed.size(), 1, 44100, channelMap);
}

// =========================================================================
//  Sprite pre-rendering with neon glow
// =========================================================================
static sf::Texture makeSprite(const char** map, int px, sf::Color color, int pad = 14)
{
    int rows = 0;
    while (map[rows])
        ++rows;

    int cols = (map && map[0]) ? (int)strlen(map[0]) : 0;
    int w = cols * px + pad * 2;
    int h = rows * px + pad * 2;

    sf::RenderTexture rt({ (unsigned)w, (unsigned)h });
    rt.clear(sf::Color(0, 0, 0, 0));

    for (int r = 0; r < rows; ++r)
    {
        bool rowEmpty = true;
        for (int c = 0; c < cols; ++c)
        {
            if (map[r][c] == '1')
            {
                rowEmpty = false;
                break;
            }
        }
        if (rowEmpty)
            continue;

        int c = 0;
        while (c < cols)
        {
            if (map[r][c] == '1')
            {
                int c2 = c;
                while (c2 < cols && map[r][c2] == '1')
                    ++c2;
                float sw = (float)((c2 - c) * px);
                float sx = (float)(pad + c * px);
                float sy = (float)(pad + r * px);

                // Outer glow
                {
                    sf::RectangleShape glow({ sw + px * 2.f, (float)px + px * 2.f });
                    glow.setPosition({ sx - px, sy - px });
                    glow.setFillColor(sf::Color(color.r, color.g, color.b, 25));
                    rt.draw(glow);
                }
                // Inner glow
                {
                    sf::RectangleShape iglow({ sw + px, (float)px + px });
                    iglow.setPosition({ sx - px * 0.5f, sy - px * 0.5f });
                    iglow.setFillColor(sf::Color(color.r, color.g, color.b, 55));
                    rt.draw(iglow);
                }
                // Bright core
                {
                    sf::RectangleShape core({ sw, (float)px });
                    core.setPosition({ sx, sy });
                    core.setFillColor(color);
                    rt.draw(core);
                }

                c = c2;
            }
            else ++c;
        }
    }

    rt.display();
    return sf::Texture(rt.getTexture());
}

// =========================================================================
//  Game
// =========================================================================
game::Game::Game(sf::RenderWindow& window)
    : m_window(window)
    , m_bgSprite(m_bgTex)
    , m_rng(std::random_device{}())
{
    //Load font (Courier New Bold -> Consolas Bold -> Arial Bold)
    if (!m_font.openFromFile("C:\\Windows\\Fonts\\courbd.ttf"))
    {
        if (!m_font.openFromFile("C:\\Windows\\Fonts\\consolab.ttf"))
        {
            [[maybe_unused]] bool unused = m_font.openFromFile("C:\\Windows\\Fonts\\arialbd.ttf");
        }
    }

    // Default player
    m_player = { kW / 2, kH - 64, 39.f, 18.f, 0, 0, 0, 0, 0, true };

    // Star field
    for (int i = 0; i < 140; ++i)
        m_stars.push_back({ randf(0, kW), randf(0, kH), randf(0.25f, 1.f), randf(0, kTAU) });

    initSprites();
    initSounds();
    loadHi();
    makeWave();

    // Pre-render background gradient + nebula
    {
        sf::RenderTexture rt({ (unsigned)kW, (unsigned)kH });
        for (int y = 0; y < (int)kH; ++y)
        {
            float t = (float)y / (float)kH;
            sf::Color c;
            if (t < 0.6f)
            {
                float f = t / 0.6f;
                c = sf::Color((unsigned char)(5 + 2 * f),
                    (unsigned char)(7 + 4 * f),
                    (unsigned char)(15 + 9 * f));
            }
            else
            {
                float f = (t - 0.6f) / 0.4f;
                c = sf::Color((unsigned char)(7 + 3 * f),
                    (unsigned char)(11 + 4 * f),
                    (unsigned char)(24 + 10 * f));
            }
            sf::RectangleShape strip({ kW, 1.f });
            strip.setPosition({ 0, (float)y });
            strip.setFillColor(c);
            rt.draw(strip);
        }

        auto nebula = [&](float cx, float cy, float radius, sf::Color col)
            {
                for (int r = (int)radius; r > 0; r -= 4)
                {
                    float a = 20.f * (1.f - (float)r / radius);
                    sf::CircleShape cs((float)r, 32);
                    cs.setPosition({ cx - (float)r, cy - (float)r });
                    cs.setFillColor(sf::Color(col.r, col.g, col.b, (unsigned char)a));
                    rt.draw(cs);
                }
            };
        nebula(kW * 0.25f, kH * 0.30f, 220.f, sf::Color(90, 40, 160));
        nebula(kW * 0.75f, kH * 0.50f, 260.f, sf::Color(20, 80, 140));
        nebula(kW * 0.50f, kH * 0.15f, 180.f, sf::Color(150, 30, 90));

        rt.display();
        m_bgTex = rt.getTexture();
        m_bgSprite.setTexture(m_bgTex);
    }
}

int game::Game::run()
{
    m_clock.restart();

    while (m_window.isOpen())
    {
        // ===================== Events =====================
        while (auto event = m_window.pollEvent())
        {
            if (event->is<sf::Event::Closed>())
            {
                m_window.close();
                return 0;
            }

            if (auto* key = event->getIf<sf::Event::KeyPressed>())
            {
                switch (key->code)
                {
                    case sf::Keyboard::Key::Enter:
                        if (m_state == State::Menu || m_state == State::Over)
                            startGame();
                        break;

                    case sf::Keyboard::Key::P:
                        if (m_state == State::Playing)
                            m_state = State::Paused;
                        else if (m_state == State::Paused)
                            m_state = State::Playing;
                        break;

                    case sf::Keyboard::Key::M:
                        m_muted = !m_muted;
                        break;

                    default: break;
                }
            }

            if (auto* mb = event->getIf<sf::Event::MouseButtonPressed>())
            {
                if (mb->button == sf::Mouse::Button::Left)
                {
                    if (m_state == State::Menu || m_state == State::Over)
                        startGame();
                }
            }
        }

        // ===================== Update =====================
        float dt = m_clock.restart().asSeconds();
        if (dt > 0.033f)
            dt = 0.033f;
        if (dt < 0.f)
            dt = 0.f;
        update(dt);

        // ===================== Render =====================
        render();
        m_window.display();
    }
    return 0;
}

// =========================================================================
//  Sprite init
// =========================================================================
void game::Game::initSprites()
{
    // Squid (11x8, 2 frames)
    static const char* sqA[] = {
        "00011111000","00111111100","01111111110","11011111011",
        "11111111111","01111111110","01100100110","01001010010", nullptr };
    static const char* sqB[] = {
        "00011111000","00111111100","01111111110","11011111011",
        "11111111111","01111111110","01001001010","10010110100", nullptr };

    // Crab (13x8, 2 frames)
    static const char* crA[] = {
        "0010000000100","0001000001000","0011111111100","0111011101110",
        "1111111111111","1011111111101","1010000000101","0110000000110", nullptr };
    static const char* crB[] = {
        "0000000000000","0100000000010","0111111111110","1111011101111",
        "1111111111111","0111111111110","0110000000110","0000000000000", nullptr };

    // Octo (12x8, 2 frames)
    static const char* ocA[] = {
        "011111111110","111111111111","111111111111","011011110110",
        "111111111111","011111111111","001100001100","011000000110", nullptr };
    static const char* ocB[] = {
        "011111111110","111111111111","111111111111","011011110110",
        "111111111111","011111111110","011000000110","001100001100", nullptr };

    // Ship (13x6, 1 frame)
    static const char* shA[] = {
        "0000001000000","0000011100000","0000111110000","0111111111110",
        "1111111111111","1111111111111", nullptr };

    // UFO (15x6, 1 frame)
    static const char* ufA[] = {
        "000111111111100","001111111111110","011111111111111","111100110011111",
        "111111111111111","001111001111000", nullptr };

    const sf::Color squidCol(255, 95, 210);
    const sf::Color crabCol(53, 224, 255);
    const sf::Color octoCol(73, 255, 155);
    const sf::Color shipCol(125, 249, 255);
    const sf::Color ufoCol(255, 77, 94);

    m_sprites["squid_0"] = makeSprite(sqA, 3, squidCol);
    m_sprites["squid_1"] = makeSprite(sqB, 3, squidCol);
    m_sprites["crab_0"] = makeSprite(crA, 3, crabCol);
    m_sprites["crab_1"] = makeSprite(crB, 3, crabCol);
    m_sprites["octo_0"] = makeSprite(ocA, 3, octoCol);
    m_sprites["octo_1"] = makeSprite(ocB, 3, octoCol);
    m_sprites["ship_0"] = makeSprite(shA, 3, shipCol);
    m_sprites["ufo_0"] = makeSprite(ufA, 3, ufoCol);
}

// =========================================================================
//  Sound init
// =========================================================================
void game::Game::initSounds()
{
    // Shoot
    m_sounds.try_emplace("shoot", genTone(880, 0.09f, 0, 0.22f, -700));

    // Hit
    m_sounds.try_emplace("hit", genNoise(0.12f, 0.35f, 2500));

    // Boom = noise + low triangle
    {
        auto n = genNoise(0.5f, 0.4f, 900);
        auto t = genTone(90, 0.4f, 3, 0.3f, -50);
        m_sounds.try_emplace("boom", mixBuffers(n, t));
    }

    // Steps
    const float stepFreqs[] = { 112, 98, 83, 74 };
    for (int i = 0; i < 4; ++i)
    {
        auto key = "step" + std::to_string(i);
        m_sounds.try_emplace(key, genTone(stepFreqs[i], 0.11f, 3, 0.5f, 0));
    }

    // UFO hum
    m_sounds.try_emplace("ufo", genTone(580, 0.12f, 1, 0.15f, 220));

    // Power-up arpeggio
    {
        const float freqs[] = { 440, 660, 880, 1320 };
        m_sounds.try_emplace("power", genArpeggio(freqs, 4, 0.09f, 0.28f));
    }

    // UFO kill
    {
        auto n = genNoise(0.4f, 0.35f, 1500);
        const float freqs[] = { 1200, 900, 700 };
        auto a = genArpeggio(freqs, 3, 0.12f, 0.32f);
        m_sounds.try_emplace("ufoKill", mixBuffers(n, a));
    }

    // Player die
    {
        auto n = genNoise(0.8f, 0.5f, 700);
        auto t = genTone(200, 0.7f, 2, 0.35f, -170);
        m_sounds.try_emplace("playerDie", mixBuffers(n, t));
    }
}

// =========================================================================
//  Wave / Bunkers
// =========================================================================
void game::Game::makeWave()
{
    m_enemies.clear(); m_bullets.clear(); m_bombs.clear(); m_pows.clear();
    m_ufo.reset();
    m_ufoTimer = randf(10, 18);
    m_ufoSnd = 0;
    m_bombTimer = 1.2f;
    m_droppedThisWave = 0;
    m_gridOx = 0; m_gridOy = 0; m_gridDir = 1; m_noteT = 0; m_note = 0;

    const int cols = 10, rows = 5;
    const float px = 3;
    const float spx = 56.f, spy = 42.f;

    // Widest sprite = crab (13 cols)
    float maxW = 13.f * px;
    float totalW = (cols - 1) * spx + maxW;
    float x0 = (kW - totalW) / 2.f;
    float y0 = 86.f;

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const char* type = (r == 0) ? "squid" : (r <= 2 ? "crab" : "octo");
            float w, h;
            if (type[0] == 's')
            {
                w = 11 * px;
                h = 8 * px;
            }
            else if (type[0] == 'c')
            {
                w = 13 * px;
                h = 8 * px;
            }
            else
            {
                w = 12 * px;
                h = 8 * px;
            }

            m_enemies.push_back({ type, c,
                x0 + c * spx + (maxW - w) / 2.f,
                y0 + r * spy,
                w, h,
                (r >= 3) ? 2 : 1,
                true });
        }
    }

    // Bunkers
    static const char* shape[] =
    {
        "001111111111100",
        "011111111111110",
        "111111111111111",
        "111111111111111",
        "111000000000111",
        "110000000000011",
        "110000000000011",
        "110000000000011",
        nullptr
    };

    const int cw = 4;
    const float bw = 15.f * cw, bh = 8.f * cw;
    m_bunkers.clear();
    const float cx[] = { kW / 2 - 270, kW / 2 - 90, kW / 2 + 90, kW / 2 + 270 };

    for (int i = 0; i < 4; ++i)
    {
        Bunker b;
        b.x = cx[i] - bw / 2.f;
        b.y = kH - 150;
        b.w = bw; b.h = bh;
        b.cw = cw;
        for (int r = 0; shape[r]; ++r)
        {
            std::vector<bool> row;
            for (int c = 0; shape[r][c]; ++c)
                row.push_back(shape[r][c] == '1');
            b.cells.push_back(row);
        }
        m_bunkers.push_back(std::move(b));
    }
}

void game::Game::startGame()
{
    m_score = 0; m_lives = 3; m_wave = 1;
    m_newHi = false; m_waveDelay = 0; m_shake = 0;
    m_player = { kW / 2, kH - 64, 39.f, 18.f, 0, 0, 0, 0, 0, true };
    m_parts.clear(); m_floats.clear();
    makeWave();
    m_state = State::Playing;
    setBanner("WAVE 1", "DEFEND THE PLANET", 1.6f);
}

void game::Game::saveHi()
{
    if (m_score > m_hi)
    {
        m_hi = m_score;
        m_newHi = true;
        std::ofstream f("ni_hi.txt");
        if (f.is_open()) f << m_hi;
    }
}

void game::Game::loadHi()
{
    m_hi = 0;
    std::ifstream f("ni_hi.txt");
    if (f.is_open())
        f >> m_hi;
}

// =========================================================================
//  Combat
// =========================================================================
void game::Game::fire()
{
    if (m_player.cd > 0) return;
    m_player.cd = (m_player.rapid > 0) ? kRapidCD : kFireCD;

    auto mk = [&](float vx)
        {
            m_bullets.push_back({ m_player.x, m_player.y - 4, vx, kBulletVy, 4, 12 });
        };

    if (m_player.spread > 0) { mk(-190); mk(0); mk(190); }
    else { mk(0); }

    playSnd("shoot");
}

void game::Game::killEnemy(Enemy& e)
{
    e.alive = false;
    float ex = e.bx + m_gridOx, ey = e.by + m_gridOy;

    int pts; sf::Color col;
    if (e.type[0] == 's')
    {
        pts = 30;
        col = sf::Color(255, 95, 210);
    }

    else if (e.type[0] == 'c')
    {
        pts = 20;
        col = sf::Color(53, 224, 255);
    }
    else
    {
        pts = 10;
        col = sf::Color(73, 255, 155);
    }

    m_score += pts;
    addFloat(ex + e.w / 2, ey, "+" + std::to_string(pts), col);
    burst(ex + e.w / 2, ey + e.h / 2, col, 18);
    playSnd("hit");
    m_shake = std::max(m_shake, 4.f);

    if (randf(0, 1) < 0.14f && m_droppedThisWave < 3)
    {
        ++m_droppedThisWave;
        dropPow(ex + e.w / 2, ey + e.h / 2);
    }
}

void game::Game::dropPow(float x, float y)
{
    static const char keys[] = "RSHBB";
    char k = keys[randi(0, 4)];
    m_pows.push_back({ x, y, randf(70, 95), k, 0 });
}

void game::Game::applyPow(char k)
{
    switch (k)
    {
        case 'R':
            m_player.rapid = 10;
            break;
        case 'S':
            m_player.spread = 8;
            break;
        case 'H':
            m_player.shield = 6;
            break;
        default:
            m_score += 1000;
            break;
    }
    addFloat(m_player.x, m_player.y - 18, puDef(k).name, puDef(k).color);
    playSnd("power");
}

void game::Game::playerHit()
{
    if (m_player.invuln > 0 || !m_player.alive) return;
    --m_lives;
    burst(m_player.x, m_player.y + 9, sf::Color(125, 249, 255), 40);
    burst(m_player.x, m_player.y + 9, sf::Color(255, 255, 255), 20);
    m_shake = 16.f;
    playSnd("playerDie");
    m_bombs.clear();

    if (m_lives <= 0)
    {
        m_player.alive = false;
        saveHi();
        m_state = State::Over;
    }
    else
    {
        m_player.x = kW / 2;
        m_player.invuln = 2.5f;
        m_player.rapid = 0;
        m_player.spread = 0;
    }
}

void game::Game::endInvasion()
{
    m_shake = 18.f;
    playSnd("playerDie");
    for (auto& e : m_enemies)
    {
        if (e.alive)
        {
            sf::Color col(255, 95, 210);
            if (e.type[0] == 'c') col = sf::Color(53, 224, 255);
            else if (e.type[0] == 'o') col = sf::Color(73, 255, 155);
            burst(e.bx + m_gridOx + e.w / 2, e.by + m_gridOy + e.h / 2, col, 10);
            e.alive = false;
        }
    }
    m_bombs.clear();
    saveHi();
    m_state = State::Over;
}

bool game::Game::hitBunker(float px, float py)
{
    for (auto& b : m_bunkers)
    {
        if (px < b.x || px > b.x + b.w || py < b.y || py > b.y + b.h)
            continue;
        int c = (int)((px - b.x) / b.cw);
        int r = (int)((py - b.y) / b.cw);
        if (r < 0 || r >= (int)b.cells.size() || c < 0 || c >= (int)b.cells[0].size())
            continue;
        if (!b.cells[r][c])
            continue;
        blastBunker(b, c, r);
        return true;
    }
    return false;
}

void game::Game::blastBunker(Bunker& b, int c, int r)
{
    static const int dirs[][2] = {
        {0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,-1},{1,-1},{-1,1}
    };
    for (auto& d : dirs)
    {
        int cc = c + d[0], rr = r + d[1];
        if (rr < 0 || rr >= (int)b.cells.size())
            continue;
        if (cc < 0 || cc >= (int)b.cells[0].size())
            continue;
        if (b.cells[rr][cc])
        {
            b.cells[rr][cc] = false;
            if (randf(0, 1) < 0.6f)
                m_parts.push_back({
                    b.x + cc * 4 + 2, b.y + rr * 4 + 2,
                    randf(-40, 40), randf(-60, 10),
                    randf(0.3f, 0.6f), 0, randf(1.5f, 3.f),
                    sf::Color(58, 255, 140) });
        }
    }
}

// =========================================================================
//  Update
// =========================================================================
void game::Game::updateFx(float dt)
{
    // ===================== Particles =====================
    for (std::size_t i = 0; i < m_parts.size(); )
    {
        auto& part = m_parts[i];
        part.t += dt;
        if (part.t >= part.life)
        {
            m_parts.erase(m_parts.begin() + i);
            continue;
        }
        part.x += part.vx * dt;
        part.y += part.vy * dt;
        part.vx *= (1.f - 1.8f * dt);
        part.vy *= (1.f - 1.8f * dt);
        ++i;
    }

    // ===================== Floating text =====================
    for (std::size_t i = 0; i < m_floats.size(); )
    {
        auto& f = m_floats[i];

        f.t += dt;
        f.y -= 28.f * dt;

        // Check if text life is over
        if (f.t >= f.life)
        {
            m_floats.erase(m_floats.begin() + i);
            continue;
        }
        ++i;
    }
}

void game::Game::moveBullets(float dt)
{
    for (std::size_t i = 0; i < m_bullets.size(); )
    {
        auto& bullet = m_bullets[i];
        bool bulletRemoved = false;

        bullet.x += bullet.vx * dt;
        bullet.y += bullet.vy * dt;

        if (bullet.y < -20 || bullet.x < 10 || bullet.x > kW - 10)
        {
            m_bullets.erase(m_bullets.begin() + i);
            continue;
        }
        if (m_ufo)
        {
            sf::FloatRect bu({ bullet.x, bullet.y }, { bullet.w, bullet.h });
            sf::FloatRect uu({ m_ufo->x, m_ufo->y }, { m_ufo->w, m_ufo->h });

            if (bu.findIntersection(uu))
            {
                int pts = 50 + randi(0, 3) * 50;
                m_score += pts;
                addFloat(m_ufo->x + m_ufo->w / 2, m_ufo->y, "+" + std::to_string(pts), sf::Color(255, 77, 94));
                burst(m_ufo->x + m_ufo->w / 2, m_ufo->y + m_ufo->h / 2, sf::Color(255, 77, 94), 30);
                m_shake = std::max(m_shake, 8.f);
                playSnd("ufoKill");
                m_ufo.reset();
                m_ufoTimer = randf(12, 22);
                m_bullets.erase(m_bullets.begin() + i);
                continue;
            }
        }
        bool hit = false;
        for (auto& e : m_enemies)
        {
            if (!e.alive) continue;
            sf::FloatRect bu({ bullet.x, bullet.y }, { bullet.w, bullet.h });
            sf::FloatRect eu({ e.bx + m_gridOx, e.by + m_gridOy }, { e.w, e.h });
            if (bu.findIntersection(eu))
            {
                --e.hp;
                if (e.hp <= 0)
                {
                    killEnemy(e);
                }
                else
                {
                    sf::Color col(255, 95, 210);
                    if (e.type[0] == 'c')
                        col = sf::Color(53, 224, 255);
                    else if (e.type[0] == 'o')
                        col = sf::Color(73, 255, 155);
                    burst(bullet.x, bullet.y, col, 4);
                    playSnd("hit");
                }
                hit = true;
                break;
            }
        }
        if (hit)
        {
            m_bullets.erase(m_bullets.begin() + i);
            continue;
        }
        if (hitBunker(bullet.x + 2, bullet.y + 2))
        {
            m_bullets.erase(m_bullets.begin() + i);
            playSnd("hit");
            continue;
        }
        ++i;
    }
}

void game::Game::update(float dt)
{
    m_gt += dt;

    // Stars scroll
    for (auto& s : m_stars)
    {
        s.y += (8 + s.z * 34) * dt;
        if (s.y > kH)
        {
            s.y = 0;
            s.x = randf(0, kW);
        }
    }

    // Shake decay
    m_shake = std::max(0.f, m_shake - 40.f * dt);

    // Banner countdown
    if (m_banner)
    {
        m_banner->t -= dt;
        if (m_banner->t <= 0)
            m_banner.reset();
    }

    if (m_state == State::Paused)
        return;
    updateFx(dt);
    if (m_state != State::Playing)
        return;

    // ===================== Player =====================
    float dir = 0;
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Left) ||
        sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A))
        dir -= 1;
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Right) ||
        sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D))
        dir += 1;

    m_player.x = clampf(m_player.x + dir * kPlayerSpd * dt, 26, kW - 26);
    m_player.cd = std::max(0.f, m_player.cd - dt);
    m_player.rapid = std::max(0.f, m_player.rapid - dt);
    m_player.spread = std::max(0.f, m_player.spread - dt);
    m_player.shield = std::max(0.f, m_player.shield - dt);
    m_player.invuln = std::max(0.f, m_player.invuln - dt);

    if (m_player.alive && sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Space))
        fire();

    // ===================== Bullets =====================
    moveBullets(dt);

    // ===================== Enemy bombs =====================
    for (std::size_t i = 0; i < m_bombs.size(); )
    {
        auto& bomb = m_bombs[i];

        bomb.y += bomb.vy * dt;

        // 1. Screen boundary check
        if (bomb.y > kH - 30)
        {
            m_bombs.erase(m_bombs.begin() + i);
            continue;
        }
        if (hitBunker(bomb.x + 2, bomb.y + 6))
        {
            m_bombs.erase(m_bombs.begin() + i);
            playSnd("hit");
            continue;
        }
        if (m_player.alive)
        {
            sf::FloatRect bb({ bomb.x, bomb.y }, { bomb.w, bomb.h });

            if (m_player.shield > 0)
            {
                sf::FloatRect sb({ m_player.x - 26.f, m_player.y - 12.f }, { 52.f, 34.f });
                if (bb.findIntersection(sb))
                {
                    m_bombs.erase(m_bombs.begin() + i);
                    burst(bomb.x + 2.f, bomb.y + 6.f, sf::Color(143, 143, 255), 8);
                    playSnd("hit");
                    continue;
                }
            }
            else if (m_player.invuln <= 0)
            {
                sf::FloatRect pb({ m_player.x - 15.f, m_player.y }, { 30.f, 18.f });
                if (bb.findIntersection(pb))
                {
                    m_bombs.erase(m_bombs.begin() + i);
                    playerHit();
                    continue;
                }
            }
        }
        ++i;
    }

    // ===================== Power-ups =====================
    for (std::size_t i = 0; i < m_pows.size(); )
    {
        auto& pow = m_pows[i];

        pow.t += dt;
        pow.y += pow.vy * dt;

        if (pow.y > kH - 40)
        {
            m_pows.erase(m_pows.begin() + i);
            continue;
        }
        if (m_player.alive)
        {
            sf::FloatRect pw({ pow.x - 13.f, pow.y - 10.f }, { 26.f, 20.f });
            sf::FloatRect pl({ m_player.x - 22.f, m_player.y - 14.f }, { 44.f, 32.f });

            if (pw.findIntersection(pl))
            {
                applyPow(pow.k);
                m_pows.erase(m_pows.begin() + i);
                continue;
            }
        }
        ++i;
    }

    // ===================== UFO =====================
    if (!m_ufo)
    {
        m_ufoTimer -= dt;
        if (m_ufoTimer <= 0)
        {
            bool fromLeft = randf(0, 1) < 0.5f;
            m_ufo = { fromLeft ? -50.f : kW + 10.f, 64.f, 45.f, 18.f,
                      fromLeft ? 130.f : -130.f };
            m_ufoSnd = 0;
        }
    }
    else
    {
        m_ufo->x += m_ufo->vx * dt;
        m_ufoSnd -= dt;
        if (m_ufoSnd <= 0)
        {
            playSnd("ufo");
            m_ufoSnd = 0.22f;
        }
        if (m_ufo->x < -70 || m_ufo->x > kW + 70)
        {
            m_ufo.reset();
            m_ufoTimer = randf(12, 22);
        }
    }

    // ===================== Enemies =====================
    int alive = 0;
    float minx = 1e9f, maxx = -1e9f, maxbottom = 0;
    for (const auto& e : m_enemies)
    {
        if (e.alive)
        {
            ++alive;
            float x = e.bx + m_gridOx;
            minx = std::min(minx, x);
            maxx = std::max(maxx, x + e.w);
            maxbottom = std::max(maxbottom, e.by + m_gridOy + e.h);
        }
    }

    if (alive == 0)
    {
        if (m_waveDelay <= 0)
        {
            m_waveDelay = 1.4f;
            int bonus = 100 * m_wave;
            m_score += bonus;
            setBanner("WAVE CLEARED", "+" + std::to_string(bonus) + " PTS BONUS", 1.4f);
            playSnd("power");
        }
        else
        {
            m_waveDelay -= dt;
            if (m_waveDelay <= 0)
            {
                ++m_wave;
                makeWave();
                setBanner("WAVE " + std::to_string(m_wave), "", 1.4f);
            }
        }
        return;
    }

    // Movement
    float frac = (float)alive / (float)m_enemies.size();
    float speed = clampf(16 + (1 - frac) * 150 + (m_wave - 1) * 12, 16, 240);
    m_gridOx += m_gridDir * speed * dt;

    if (minx < 26 || maxx > kW - 26)
    {
        m_gridDir *= -1;
        m_gridOy += 18;
        // Re-clamp
        float mnx = 1e9f, mxx = -1e9f;
        for (const auto& e : m_enemies)
        {
            if (e.alive)
            {
                float x = e.bx + m_gridOx;
                mnx = std::min(mnx, x);
                mxx = std::max(mxx, x + e.w);
            }
        }
        if (mnx < 26)
            m_gridOx += 26 - mnx;
        else if (mxx > kW - 26)
            m_gridOx -= mxx - (kW - 26);
    }

    // Step sound
    m_noteT -= dt;
    if (m_noteT <= 0)
    {
        playSnd("step" + std::to_string(m_note % 4));
        ++m_note;
        m_noteT = clampf(0.62f - speed / 260.f, 0.1f, 0.62f);
    }

    // Invasion
    if (maxbottom >= m_player.y - 2)
    {
        endInvasion();
        return;
    }

    // Enemies smash bunkers
    for (auto& b : m_bunkers)
    {
        bool any = false;
        for (const auto& row : b.cells)
        {
            for (bool cell : row)
            {
                if (cell)
                    any = true;
                break;
            }
        }
        if (any)
            break;
        if (!any)
            continue;

        bool smashed = false;
        for (const auto& e : m_enemies)
        {
            if (!e.alive) continue;
            float ex = e.bx + m_gridOx, ey = e.by + m_gridOy;
            if (ey + e.h >= b.y && ex < b.x + b.w && ex + e.w > b.x)
            {
                // Particles from some cells
                for (size_t r = 0; r < b.cells.size(); ++r)
                {
                    for (size_t c = 0; c < b.cells[r].size(); ++c)
                    {
                        if (b.cells[r][c] && randf(0, 1) < 0.3f)
                            burst(b.x + c * b.cw + 2, b.y + r * b.cw + 2, sf::Color(58, 255, 140), 2);
                    }
                }

                for (auto& row : b.cells)
                {
                    for (auto&& cell : row)
                        cell = false;
                }

                m_shake = std::max(m_shake, 8.f);
                playSnd("boom");
                smashed = true;
                break;
            }
        }
        (void)smashed;
    }

    // Enemy bombing
    m_bombTimer -= dt;
    if (m_bombTimer <= 0)
    {
        // Bottom enemy per column
        std::map<int, const Enemy*> bottom;
        for (const auto& e : m_enemies)
        {
            if (e.alive)
            {
                auto it = bottom.find(e.col);
                if (it == bottom.end() || e.by > it->second->by)
                    bottom[e.col] = &e;
            }
        }

        if (!bottom.empty())
        {
            auto it = bottom.begin();
            std::advance(it, randi(0, (int)bottom.size() - 1));
            const Enemy& e = *it->second;
            m_bombs.push_back({
                e.bx + m_gridOx + e.w / 2 - 2,
                e.by + m_gridOy + e.h,
                4, 12,
                std::min(430.f, 170.f + m_wave * 14.f)
                });
        }

        m_bombTimer = std::max(0.25f,
            randf(0.55f, 1.4f) *
            clampf(0.35f + (alive / 50) * 0.85f, 0.35f, 1.2f) *
            std::max(0.5f, 1.f - (m_wave - 1) * 0.08f));
    }
}

// =========================================================================
//  Render helpers
// =========================================================================
void game::Game::drawText(const std::string& str, float x, float y, float size,
    sf::Color color, const char* align, float glow)
{
    // Compute position
    sf::Text t(m_font);
    t.setString(str);
    t.setCharacterSize((unsigned)size);
    t.setStyle(sf::Text::Style::Bold); // 

    sf::FloatRect b = t.getLocalBounds();
    float tx = x, ty = y;

    if (align[0] == 'c')
        tx = x - b.size.x / 2.f - b.position.x;
    else if (align[0] == 'r')
        tx = x - b.size.x - b.position.x;

    // Glow (draw 4 offset copies with low alpha)
    if (glow > 0)
    {
        sf::Color gc(color.r, color.g, color.b, 50);
        sf::Text gt(m_font);
        gt.setString(str);
        gt.setCharacterSize((unsigned)size);
        gt.setStyle(sf::Text::Style::Bold);
        gt.setFillColor(gc);

        const float offs[] = { -1.f, 1.f };
        for (float dx : offs)
        {
            for (float dy : offs)
            {
                gt.setPosition({ tx + dx, ty + dy });
                m_window.draw(gt);
            }
        }
    }

    t.setFillColor(color);
    t.setPosition({ tx, ty });
    m_window.draw(t);
}

// =========================================================================
//  Render
// =========================================================================
void game::Game::drawBackground()
{
    m_window.draw(m_bgSprite);

    // Stars
    for (const auto& s : m_stars)
    {
        float a = (0.35f + 0.65f * std::abs(sinf(m_gt * 1.5f + s.tw))) * s.z;
        bool bright = s.z > 0.7f;
        sf::Color c = bright ? sf::Color(207, 232, 255) : sf::Color(138, 168, 216);
        c.a = (unsigned char)(255 * a);
        float ss = bright ? 2.f : 1.f;
        sf::RectangleShape star(sf::Vector2f(ss, ss));
        star.setPosition({ s.x, s.y });
        star.setFillColor(c);
        m_window.draw(star);
    }

    // Neon ground line
    sf::RectangleShape line(sf::Vector2f(kW, 2.f));
    line.setPosition({ 0, kH - 40 });
    line.setFillColor(sf::Color(25, 224, 122));
    m_window.draw(line);
}

void game::Game::drawBunkers()
{
    sf::RectangleShape cell(sf::Vector2f(4, 4));
    cell.setFillColor(sf::Color(25, 224, 122));
    for (const auto& b : m_bunkers)
    {
        for (size_t r = 0; r < b.cells.size(); ++r)
        {
            for (size_t c = 0; c < b.cells[r].size(); ++c)
            {
                if (b.cells[r][c])
                {
                    cell.setPosition({ b.x + c * b.cw, b.y + r * b.cw });
                    m_window.draw(cell);
                }
            }
        }
    }
}

void game::Game::drawEnemies()
{
    int frame = (int)(m_gt * 3) % 2;

    for (const auto& e : m_enemies)
    {
        if (!e.alive)
            continue;

        char key[32];
        snprintf(key, sizeof(key), "%s_%d", e.type, frame);

        auto it = m_sprites.find(key);
        if (it == m_sprites.end())
            continue;

        sf::Sprite sprite(it->second);

        // 14 px padding baked into texture
        sprite.setPosition({ e.bx + m_gridOx - 14.f, e.by + m_gridOy - 14.f });
        m_window.draw(sprite);
    }
}

void game::Game::drawPlayer()
{
    if (m_state == State::Menu || !m_player.alive) return;
    if (m_player.invuln > 0 && (int)(m_gt * 12) % 2 == 0) return;

    sf::Sprite sprite(m_sprites["ship_0"]);
    sprite.setPosition({ m_player.x - 19.5f - 14.f, m_player.y - 14.f });
    m_window.draw(sprite);

    // Shield ring
    if (m_player.shield > 0)
    {
        float a = 0.35f + 0.25f * sinf(m_gt * 6.f);

        sf::CircleShape shield(30.f, 48);
        shield.setPosition({ m_player.x - 30.f, m_player.y + 9.f - 30.f });
        shield.setOutlineColor(sf::Color(143, 143, 255, (unsigned char)(255 * a)));
        shield.setOutlineThickness(2.f);
        shield.setFillColor(sf::Color(0, 0, 0, 0));
        m_window.draw(shield);
    }
}

void game::Game::drawUfo()
{
    if (!m_ufo)
        return;
    sf::Sprite sprite(m_sprites["ufo_0"]);
    sprite.setPosition({ m_ufo->x - 14.f, m_ufo->y - 14.f });
    m_window.draw(sprite);
}

void game::Game::drawBullets()
{
    sf::RectangleShape b(sf::Vector2f(4, 12));
    b.setFillColor(sf::Color(234, 255, 255));
    for (const auto& bl : m_bullets)
    {
        b.setPosition({ bl.x - 2, bl.y });
        m_window.draw(b);
    }
}

void game::Game::drawBombs()
{
    sf::RectangleShape b(sf::Vector2f(4, 12));
    b.setFillColor(sf::Color(255, 210, 62));
    for (const auto& bm : m_bombs)
    {
        b.setPosition({ bm.x, bm.y });
        m_window.draw(b);
    }
}

void game::Game::drawPows()
{
    for (const auto& p : m_pows)
    {
        sf::Color c = puDef(p.k).color;
        float py = p.y + sinf(p.t * 4.f) * 2.f;

        sf::RectangleShape box(sf::Vector2f(26, 20));
        box.setPosition({ p.x - 13, py - 10 });
        box.setFillColor(sf::Color(10, 14, 30, 230));
        box.setOutlineColor(c);
        box.setOutlineThickness(2);
        m_window.draw(box);

        drawText(std::string(1, p.k), p.x, py - 6, 13, c, "c", 0);
    }
}

void game::Game::drawParticles()
{
    for (const auto& p : m_parts)
    {
        float a = std::max(0.f, 1.f - p.t / p.life);
        sf::RectangleShape s(sf::Vector2f(p.size, p.size));
        s.setPosition({ p.x - p.size / 2, p.y - p.size / 2 });
        sf::Color c = p.color;
        c.a = (unsigned char)(255 * a);
        s.setFillColor(c);
        m_window.draw(s);
    }
}

void game::Game::drawFloats()
{
    for (const auto& f : m_floats)
    {
        float a = std::max(0.f, 1.f - f.t / f.life);
        sf::Color c = f.color;
        c.a = (unsigned char)(255 * a);
        drawText(f.txt, f.x, f.y, 13, c, "c", 0);
    }
}

void game::Game::drawHud()
{
    if (m_state == State::Menu) return;

    const sf::Color labelCol(127, 159, 200);

    drawText("SCORE", 16, 12, 11, labelCol, "l", 0);
    drawText(padInt(m_score, 6), 16, 26, 18, sf::Color(255, 255, 255), "l", 6);

    drawText("HI-SCORE", kW / 2, 12, 11, labelCol, "c", 0);
    drawText(padInt(m_hi, 6), kW / 2, 26, 18, sf::Color(255, 210, 62), "c", 6);

    drawText("WAVE " + padInt(m_wave, 2), kW - 16, 12, 11, labelCol, "r", 0);
    drawText(padInt(m_wave, 2), kW - 16, 26, 18, sf::Color(255, 255, 255), "r", 6);

    // Lives (small ship icons)
    sf::Sprite lifeIcon(m_sprites["ship_0"]);
    lifeIcon.setScale({ 0.5f, 0.5f });
    lifeIcon.setOrigin({ 7.f, 7.f });

    for (int i = 0; i < m_lives; ++i)
    {
        lifeIcon.setPosition({ 16.f + i * 26.f, kH - 26.f });
        m_window.draw(lifeIcon);
    }

    // Active power-up timers
    float px = kW - 16;
    if (m_player.rapid > 0)
    {
        drawText("R " + std::to_string((int)m_player.rapid) + "s", px, kH - 24, 12, sf::Color(53, 224, 255), "r", 4);
        px -= 52;
    }
    if (m_player.spread > 0)
    {
        drawText("S " + std::to_string((int)m_player.spread) + "s", px, kH - 24, 12, sf::Color(73, 255, 155), "r", 4);
        px -= 52;
    }
    if (m_player.shield > 0)
    {
        drawText("H " + std::to_string((int)m_player.shield) + "s", px, kH - 24, 12, sf::Color(143, 143, 255), "r", 4);
        px -= 52;
    }

    if (m_muted)
        drawText("MUTED [M]", kW - 16, 52, 10, sf::Color(90, 107, 133), "r", 0);
}

void game::Game::drawOverlays()
{
    const float t = m_gt;

    // ---- Banner ----
    if (m_banner)
    {
        float alpha = std::min(1.f, m_banner->t * 2.5f);
        sf::Color bc(255, 255, 255, (unsigned char)(255 * alpha));
        drawText(m_banner->text, kW / 2, kH * 0.38f, 34, bc, "c", 18);
        if (!m_banner->sub.empty())
        {
            sf::Color sc(159, 232, 255, (unsigned char)(255 * alpha));
            drawText(m_banner->sub, kW / 2, kH * 0.38f + 44, 15, sc, "c", 8);
        }
    }

    // ---- Menu ----
    if (m_state == State::Menu)
    {
        sf::RectangleShape dim(sf::Vector2f(kW, kH));
        dim.setFillColor(sf::Color(3, 5, 12, 184));
        m_window.draw(dim);

        drawText("NEON INVADERS", kW / 2, 110, 46, sf::Color(255, 255, 255), "c", 24);
        drawText("A   R E T R O   S P A C E   D E F E N S E", kW / 2, 172, 13, sf::Color(125, 249, 255), "c", 8);

        // Demo sprites
        int frame = (int)(t * 3) % 2;
        struct Demo { const char* name; float x; const char* pts; float w; sf::Color col; };
        Demo demos[] = {
            { "squid", kW / 2 - 210, "30", 22, sf::Color(255,95,210) },
            { "crab",  kW / 2 - 110, "20", 26, sf::Color(53,224,255) },
            { "octo",  kW / 2 - 10,  "10", 24, sf::Color(73,255,155) },
            { "ufo",   kW / 2 + 90,  "???",30, sf::Color(255,77,94) },
        };
        for (const auto& d : demos)
        {
            char key[32];
            snprintf(key, sizeof(key), "%s_%d", d.name, frame);
            auto it = m_sprites.find(key);
            if (it != m_sprites.end())
            {
                sf::Sprite s(it->second);
                s.setScale({ 0.66f, 0.66f });
                s.setOrigin({ 14.f * 0.66f, 14.f * 0.66f });
                s.setPosition({ d.x, 220.f });
                m_window.draw(s);
            }
            drawText(d.pts, d.x + d.w / 2.f, 248.f, 12.f, d.col, "c", 6.f);
        }

        drawText("ARROWS / A D   MOVE", kW / 2, 310, 13, sf::Color(159, 180, 216), "c", 0);
        drawText("SPACE            FIRE (HOLD)", kW / 2, 334, 13, sf::Color(159, 180, 216), "c", 0);
        drawText("P  PAUSE     M  SOUND", kW / 2, 358, 13, sf::Color(159, 180, 216), "c", 0);
        drawText("HI-SCORE  " + padInt(m_hi, 6), kW / 2, 412, 15, sf::Color(255, 210, 62), "c", 6);

        if ((int)(t * 2.5f) % 2 == 0)
            drawText("PRESS ENTER OR CLICK TO START", kW / 2, 470, 16, sf::Color(255, 255, 255), "c", 10);

        drawText("C A N V A S  E D I T I O N   v1.0", kW / 2, 580, 10, sf::Color(51, 65, 94), "c", 0);
    }

    // ---- Game Over ----
    if (m_state == State::Over)
    {
        sf::RectangleShape dim(sf::Vector2f(kW, kH));
        dim.setFillColor(sf::Color(3, 5, 12, 184));
        m_window.draw(dim);

        drawText("GAME OVER", kW / 2, 170, 54, sf::Color(255, 77, 94), "c", 26);
        drawText("FINAL SCORE  " + std::to_string(m_score), kW / 2, 258, 20, sf::Color(255, 255, 255), "c", 8);

        if (m_newHi)
            drawText("* NEW HI-SCORE! *", kW / 2, 300, 16, sf::Color(255, 210, 62), "c", 10);
        else
            drawText("HI-SCORE  " + std::to_string(m_hi), kW / 2, 300, 16, sf::Color(159, 232, 255), "c", 6);
        if ((int)(t * 2) % 2 == 0)
            drawText("PRESS ENTER TO PLAY AGAIN", kW / 2, 380, 15, sf::Color(255, 255, 255), "c", 8);
    }

    // ---- Paused ----
    if (m_state == State::Paused)
    {
        sf::RectangleShape dim(sf::Vector2f(kW, kH));
        dim.setFillColor(sf::Color(3, 5, 12, 184));
        m_window.draw(dim);
        drawText("PAUSED", kW / 2, kH / 2 - 30, 42, sf::Color(125, 249, 255), "c", 18);
        drawText("PRESS P TO RESUME", kW / 2, kH / 2 + 30, 13, sf::Color(159, 232, 255), "c", 6);
    }
}

void game::Game::render()
{
    // Screen shake via view offset
    if (m_shake > 0.5f)
    {
        sf::View view = m_window.getDefaultView();
        sf::Vector2f center = view.getCenter();
        center.x += randf(-m_shake, m_shake) * 0.5f;
        center.y += randf(-m_shake, m_shake) * 0.5f;
        view.setCenter(center);
        m_window.setView(view);
    }

    m_window.clear();
    drawBackground();
    drawBunkers();
    drawEnemies();
    drawUfo();
    drawBullets();
    drawBombs();
    drawPows();
    drawPlayer();
    drawParticles();
    drawFloats();
    drawHud();
    drawOverlays();
    m_window.setView(m_window.getDefaultView());
}

// =========================================================================
//  Helpers
// =========================================================================
void game::Game::addFloat(float x, float y, const std::string& txt, sf::Color color)
{
    m_floats.push_back({ x, y, txt, color, 1.1f, 0 });
}

void game::Game::burst(float x, float y, sf::Color color, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float a = randf(0, kTAU);
        float s = randf(40, 240);
        m_parts.push_back({
            x, y,
            cosf(a) * s, sinf(a) * s,
            randf(0.35f, 0.8f), 0,
            randf(1.5f, 3.5f),
            color
            });
    }
}

void game::Game::setBanner(const std::string& text, const std::string& sub, float t)
{
    m_banner = Banner{ text, sub, t };
}

float game::Game::randf(float a, float b)
{
    std::uniform_real_distribution<float> dist(a, b);
    return dist(m_rng);
}

int game::Game::randi(int a, int b)
{
    std::uniform_int_distribution<int> dist(a, b);
    return dist(m_rng);
}

void game::Game::playSnd(const std::string& name) {
    if (m_muted) return;

    auto soundIt = m_sounds.find(name);
    if (soundIt == m_sounds.end()) return;

    soundIt->second.sound.play();
}

bool game::Game::overlap(const Bullet& a, const Bullet& b)
{
    return sf::FloatRect({ a.x, a.y }, { a.w, a.h }).findIntersection(
        sf::FloatRect({ b.x, b.y }, { b.w, b.h })).has_value();
}

bool game::Game::overlap(const sf::FloatRect& a, const sf::FloatRect& b)
{
    return static_cast<bool>(a.findIntersection(b));
}

// =========================================================================
//  Theme & Custom Color Window Styling Configuration
// =========================================================================
static void configureGameWindowTheme(sf::WindowHandle handle)
{
    HWND hwnd = static_cast<HWND>(handle);

    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    HDC hdc = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdc);

    HBITMAP hColorBmp = CreateCompatibleBitmap(hdc, 16, 16);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hColorBmp);

    HBRUSH hBrush = CreateSolidBrush(RGB(255, 95, 210));
    RECT rect = { 0, 0, 16, 16 };
    FillRect(hdcMem, &rect, hBrush);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBrush);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdc);

    HBITMAP hMaskBmp = CreateBitmap(16, 16, 1, 1, nullptr);

    ICONINFO iconInfo = { 0 };
    iconInfo.fIcon = TRUE;
    iconInfo.xHotspot = 0;
    iconInfo.yHotspot = 0;
    iconInfo.hbmMask = hMaskBmp;
    iconInfo.hbmColor = hColorBmp;

    HICON hCustomColorIcon = CreateIconIndirect(&iconInfo);

    if (hCustomColorIcon)
    {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hCustomColorIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hCustomColorIcon);
    }

    DeleteObject(hColorBmp);
    DeleteObject(hMaskBmp);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// =========================================================================
//  wWinMain Entry Point
// =========================================================================
int wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    sf::RenderWindow window(
        sf::VideoMode({ static_cast<unsigned int>(game::kW), static_cast<unsigned int>(game::kH) }),
        L"NEON INVADERS"
    );

    window.setFramerateLimit(60);
    configureGameWindowTheme(window.getNativeHandle());
    window.setKeyRepeatEnabled(false);

    game::Game game(window);
    return game.run();
}
