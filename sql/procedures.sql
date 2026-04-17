-- llmlog stored procedures
-- All PSQL is idempotent: CREATE OR ALTER is used throughout so re-running
-- this file on an existing DB updates procedure bodies without error.

SET TERM ^ ;

-- ---------------------------------------------------------------------------
-- SP_GET_EFFECTIVE_PRICING
-- Returns the price row whose EFFECTIVE_FROM is the latest not exceeding
-- CALL_TIME. NULL IN_RATE is a hard error (unpriced model at the call time).
-- ---------------------------------------------------------------------------
CREATE OR ALTER PROCEDURE SP_GET_EFFECTIVE_PRICING (
    MODEL_ID   INTEGER,
    CALL_TIME  TIMESTAMP WITH TIME ZONE
) RETURNS (
    INPUT_PER_MTOK       DECFLOAT(34),
    OUTPUT_PER_MTOK      DECFLOAT(34),
    CACHE_READ_PER_MTOK  DECFLOAT(34),
    CACHE_WRITE_PER_MTOK DECFLOAT(34),
    IMAGE_PER_MTOK       DECFLOAT(34)
) AS
BEGIN
    SELECT FIRST 1
           INPUT_PER_MTOK,
           OUTPUT_PER_MTOK,
           CACHE_READ_PER_MTOK,
           CACHE_WRITE_PER_MTOK,
           IMAGE_PER_MTOK
      FROM PRICING
     WHERE MODEL_ID = :MODEL_ID
       AND EFFECTIVE_FROM <= :CALL_TIME
     ORDER BY EFFECTIVE_FROM DESC
    INTO :INPUT_PER_MTOK,
         :OUTPUT_PER_MTOK,
         :CACHE_READ_PER_MTOK,
         :CACHE_WRITE_PER_MTOK,
         :IMAGE_PER_MTOK;

    IF (INPUT_PER_MTOK IS NULL) THEN
        EXCEPTION EX_PRICING_NOT_FOUND;

    SUSPEND;
END ^

-- ---------------------------------------------------------------------------
-- SP_LOG_REQUEST
-- Single-call commit path: compute exact cost in DECFLOAT and insert a
-- REQUESTS row. Invoked once per upstream API call from the proxy.
-- ---------------------------------------------------------------------------
CREATE OR ALTER PROCEDURE SP_LOG_REQUEST (
    CALL_TIME       TIMESTAMP WITH TIME ZONE,
    MODEL_ID        INTEGER,
    REQUEST_ID      VARCHAR(96),
    INPUT_TOKENS    BIGINT,
    OUTPUT_TOKENS   BIGINT,
    CACHE_READ      BIGINT,
    CACHE_WRITE     BIGINT,
    IMAGE_TOKENS    BIGINT,
    LATENCY_MS      INTEGER,
    HTTP_STATUS     SMALLINT,
    TAG             VARCHAR(64)
) RETURNS (
    ID       BIGINT,
    COST_USD DECFLOAT(34)
) AS
    DECLARE VARIABLE IN_RATE  DECFLOAT(34);
    DECLARE VARIABLE OUT_RATE DECFLOAT(34);
    DECLARE VARIABLE CR_RATE  DECFLOAT(34);
    DECLARE VARIABLE CW_RATE  DECFLOAT(34);
    DECLARE VARIABLE IMG_RATE DECFLOAT(34);
BEGIN
    SELECT INPUT_PER_MTOK,
           OUTPUT_PER_MTOK,
           CACHE_READ_PER_MTOK,
           CACHE_WRITE_PER_MTOK,
           IMAGE_PER_MTOK
      FROM SP_GET_EFFECTIVE_PRICING(:MODEL_ID, :CALL_TIME)
      INTO :IN_RATE, :OUT_RATE, :CR_RATE, :CW_RATE, :IMG_RATE;

    -- Exact DECFLOAT arithmetic: (tokens * rate) / 1,000,000. Division last
    -- keeps all intermediate precision.
    COST_USD =   (:IN_RATE  *     :INPUT_TOKENS) / 1000000
               + (:OUT_RATE *    :OUTPUT_TOKENS) / 1000000
               + (COALESCE(:CR_RATE,  0) *   :CACHE_READ)   / 1000000
               + (COALESCE(:CW_RATE,  0) *   :CACHE_WRITE)  / 1000000
               + (COALESCE(:IMG_RATE, 0) *   :IMAGE_TOKENS) / 1000000;

    INSERT INTO REQUESTS
      (CALL_TIME,  MODEL_ID,  REQUEST_ID,
       INPUT_TOKENS, OUTPUT_TOKENS, CACHE_READ, CACHE_WRITE, IMAGE_TOKENS,
       LATENCY_MS, HTTP_STATUS, COST_USD,   TAG)
    VALUES
      (:CALL_TIME, :MODEL_ID, :REQUEST_ID,
       :INPUT_TOKENS, :OUTPUT_TOKENS, :CACHE_READ, :CACHE_WRITE, :IMAGE_TOKENS,
       :LATENCY_MS, :HTTP_STATUS, :COST_USD, :TAG)
    RETURNING ID INTO :ID;

    SUSPEND;
END ^

-- ---------------------------------------------------------------------------
-- SP_REPORT_BY_MODEL
-- Aggregate cost over a window, grouped by provider + model. Sorted by cost.
-- ---------------------------------------------------------------------------
CREATE OR ALTER PROCEDURE SP_REPORT_BY_MODEL (
    FROM_TIME TIMESTAMP WITH TIME ZONE,
    TO_TIME   TIMESTAMP WITH TIME ZONE
) RETURNS (
    PROVIDER      VARCHAR(32),
    MODEL         VARCHAR(96),
    CALLS         BIGINT,
    TOTAL_INPUT   BIGINT,
    TOTAL_OUTPUT  BIGINT,
    TOTAL_CACHE_R BIGINT,
    TOTAL_CACHE_W BIGINT,
    TOTAL_COST    DECFLOAT(34)
) AS
BEGIN
    FOR
        SELECT P.NAME,
               M.MODEL_ID,
               COUNT(*),
               SUM(R.INPUT_TOKENS),
               SUM(R.OUTPUT_TOKENS),
               SUM(R.CACHE_READ),
               SUM(R.CACHE_WRITE),
               SUM(R.COST_USD)
          FROM REQUESTS R
          JOIN MODELS M   ON M.ID = R.MODEL_ID
          JOIN PROVIDERS P ON P.ID = M.PROVIDER_ID
         WHERE R.CALL_TIME >= :FROM_TIME
           AND R.CALL_TIME <  :TO_TIME
         GROUP BY P.NAME, M.MODEL_ID
         ORDER BY 8 DESC
        INTO :PROVIDER, :MODEL, :CALLS,
             :TOTAL_INPUT, :TOTAL_OUTPUT, :TOTAL_CACHE_R, :TOTAL_CACHE_W,
             :TOTAL_COST
    DO
        SUSPEND;
END ^

SET TERM ; ^
