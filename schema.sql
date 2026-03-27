-- ============================================================
-- Traffic Light System – Supabase Database Schema
-- Run this in your Supabase SQL Editor
-- ============================================================

-- ── 1. Devices table ─────────────────────────────────────
-- Tracks each physical ESP32 device
CREATE TABLE IF NOT EXISTS devices (
  id          uuid          PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id   text          UNIQUE NOT NULL,          -- e.g. "ESP32_TL_001"
  name        text          NOT NULL DEFAULT 'Traffic Light',
  location    text,                                   -- e.g. "Main Intersection"
  online      boolean       NOT NULL DEFAULT false,
  last_seen   timestamptz   DEFAULT now(),
  created_at  timestamptz   NOT NULL DEFAULT now()
);

-- Auto-update last_seen whenever online is patched
CREATE OR REPLACE FUNCTION update_last_seen()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
  NEW.last_seen = now();
  RETURN NEW;
END;
$$;

CREATE OR REPLACE TRIGGER trg_devices_last_seen
  BEFORE UPDATE OF online ON devices
  FOR EACH ROW EXECUTE FUNCTION update_last_seen();

-- ── 2. Device config table ────────────────────────────────
-- One row per device – dashboard reads/writes here; ESP32 polls it
CREATE TABLE IF NOT EXISTS device_config (
  id              uuid    PRIMARY KEY DEFAULT gen_random_uuid(),
  device_id       text    UNIQUE NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,

  -- Operational mode
  mode            text    NOT NULL DEFAULT 'auto'
                          CHECK (mode IN ('auto', 'manual')),
  enabled         boolean NOT NULL DEFAULT true,

  -- Manual mode: which light to force on
  manual_light    text    NOT NULL DEFAULT 'red'
                          CHECK (manual_light IN ('red', 'yellow', 'green', 'off')),

  -- Auto mode: duration of each phase in seconds
  red_duration    int     NOT NULL DEFAULT 30 CHECK (red_duration    BETWEEN 1 AND 300),
  yellow_duration int     NOT NULL DEFAULT 5  CHECK (yellow_duration BETWEEN 1 AND 60),
  green_duration  int     NOT NULL DEFAULT 25 CHECK (green_duration  BETWEEN 1 AND 300),

  -- Current light (written back by the ESP32, read by dashboard)
  current_light   text    NOT NULL DEFAULT 'red'
                          CHECK (current_light IN ('red', 'yellow', 'green', 'off')),

  updated_at      timestamptz NOT NULL DEFAULT now()
);

-- Auto-update updated_at
CREATE OR REPLACE FUNCTION touch_updated_at()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
  NEW.updated_at = now();
  RETURN NEW;
END;
$$;

CREATE OR REPLACE TRIGGER trg_config_updated_at
  BEFORE UPDATE ON device_config
  FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- ── 3. Traffic log table ──────────────────────────────────
-- Append-only history of every light change
CREATE TABLE IF NOT EXISTS traffic_log (
  id          bigserial     PRIMARY KEY,
  device_id   text          NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  light       text          NOT NULL CHECK (light IN ('red', 'yellow', 'green', 'off')),
  mode        text          NOT NULL CHECK (mode IN ('auto', 'manual')),
  source      text          NOT NULL DEFAULT 'device'
                            CHECK (source IN ('device', 'dashboard')),
  created_at  timestamptz   NOT NULL DEFAULT now()
);

-- Index for fast time-range queries
CREATE INDEX IF NOT EXISTS idx_traffic_log_device_time
  ON traffic_log (device_id, created_at DESC);

-- ── 4. Seed a default device ──────────────────────────────
INSERT INTO devices (device_id, name, location)
VALUES ('ESP32_TL_001', 'Traffic Light #1', 'Main Intersection')
ON CONFLICT (device_id) DO NOTHING;

INSERT INTO device_config (device_id)
VALUES ('ESP32_TL_001')
ON CONFLICT (device_id) DO NOTHING;

-- ── 5. Row Level Security (RLS) ───────────────────────────
-- Enable RLS but allow anon key full access (tighten for production!)
ALTER TABLE devices       ENABLE ROW LEVEL SECURITY;
ALTER TABLE device_config ENABLE ROW LEVEL SECURITY;
ALTER TABLE traffic_log   ENABLE ROW LEVEL SECURITY;

CREATE POLICY "anon_all_devices"       ON devices       FOR ALL TO anon USING (true) WITH CHECK (true);
CREATE POLICY "anon_all_device_config" ON device_config FOR ALL TO anon USING (true) WITH CHECK (true);
CREATE POLICY "anon_all_traffic_log"   ON traffic_log   FOR ALL TO anon USING (true) WITH CHECK (true);

-- ── 6. Enable Realtime ────────────────────────────────────
-- Run this in the Supabase Dashboard → Database → Replication
-- or uncomment below:
-- ALTER PUBLICATION supabase_realtime ADD TABLE device_config;
-- ALTER PUBLICATION supabase_realtime ADD TABLE devices;

-- ── Useful views ─────────────────────────────────────────
CREATE OR REPLACE VIEW v_device_status AS
SELECT
  d.device_id,
  d.name,
  d.location,
  d.online,
  d.last_seen,
  c.mode,
  c.enabled,
  c.current_light,
  c.manual_light,
  c.red_duration,
  c.yellow_duration,
  c.green_duration,
  c.updated_at AS config_updated_at
FROM devices d
JOIN device_config c USING (device_id);
