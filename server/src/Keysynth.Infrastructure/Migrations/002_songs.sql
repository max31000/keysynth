-- keysynth schema v2: songs / setlists (docs/specs/2026-10-12-cloud-api.md §3.3a, §5.2). Same shape as presets.

CREATE TABLE songs (
  user_id     BIGINT        NOT NULL,
  slug        VARCHAR(64)   CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  revision    INT UNSIGNED  NOT NULL,
  updated_at  DATETIME(3)   NOT NULL,
  deleted     TINYINT(1)    NOT NULL DEFAULT 0,
  song        MEDIUMTEXT    NULL,
  PRIMARY KEY (user_id, slug),
  CONSTRAINT fk_songs_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
