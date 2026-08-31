import { describe, expect, it } from "vitest";
import { formatReading } from "./client";

describe("formatReading", () => {
  it("does not confuse an absent sensor with zero", () => {
    expect(formatReading({ available: false, value: 0 }, "L/min")).toBe("Not installed");
  });

  it("renders a real zero when the sensor is available", () => {
    expect(formatReading({ available: true, value: 0 }, "L/min")).toBe("0 L/min");
  });
});
