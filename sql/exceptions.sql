-- Named exceptions raised from PSQL procedures.
-- Applied separately so procedures can reference them without ordering issues.

CREATE EXCEPTION EX_PRICING_NOT_FOUND
    'no effective pricing row found for the given model and call time';
