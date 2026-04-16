-- Named exceptions raised from PSQL procedures.
-- Applied separately so procedures can reference them without ordering issues.
-- CREATE OR ALTER means re-running bootstrap updates the message text without
-- touching the exception identity referenced by existing procedure bodies.

CREATE OR ALTER EXCEPTION EX_PRICING_NOT_FOUND
    'no effective pricing row found for the given model and call time';
